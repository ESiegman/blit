// Part of the build's output read for problems (buildscan.hpp): Linkers and assemblers (ld, collect2, GNU as), HDL tools (Verilator, Yosys, Vivado, Icarus, vvp, verible-lint, nextpnr), shader compilers (glslang, qsb, spirv-val, Metal, slangc, DXC, naga, tint) and Python's tools (Ruff, pyright, pre-commit).
//
// A line is looked at by its first byte, its first word (a command make
// echoed) and its first ':' (a place, "path:line", or a program's name,
// "ld:"); one that is none of these costs a few compares. A problem whose
// lines go on under it (Verilator's notes, pyright's message, macOS's ld
// listing a symbol's uses) is held until a line that is not part of it.
// What names no file is placed at the tool's name ("ld", "yosys"): listed,
// with nowhere to go.
#include "buildscan.hpp"

#include <algorithm>

namespace blit {

namespace {

constexpr size_t kMaxJoined = 600;  // bytes of a message put together from lines
constexpr size_t kMaxNotes = 32;    // a problem's, at most

bool digit(char c) { return c >= '0' && c <= '9'; }
bool upper(char c) { return c >= 'A' && c <= 'Z'; }
bool lower(char c) { return c >= 'a' && c <= 'z'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// The digits at s[*at] as a number, *at moved past them; false if there are none.
bool number_at(std::string_view s, size_t* at, u32* n) {
    size_t i = *at;
    u64 v = 0;
    while (i < s.size() && digit(s[i])) v = std::min<u64>(v * 10 + static_cast<u64>(s[i++] - '0'), 0xFFFFFFFFu);
    if (i == *at) return false;
    *n = static_cast<u32>(v);
    *at = i;
    return true;
}

// A place at the start of s, at its first ':': "path:line" or
// "path:line:col", and Verible's range after it ("-15", "-2:3"). rest: what
// follows. spaces: whether the path may have them.
struct Place {
    std::string_view path, rest;
    u32 line = 0, col = 0;
};
bool place_at_start(std::string_view s, Place* p, bool spaces = true) {
    const size_t c = s.find(':');
    if (c == 0 || c == std::string_view::npos || c + 1 >= s.size() || !digit(s[c + 1]) || s[0] == ' ') return false;
    p->path = s.substr(0, c);
    if (!spaces && p->path.find(' ') != std::string_view::npos) return false;
    size_t i = c + 1;
    number_at(s, &i, &p->line);
    p->col = 0;
    if (i + 1 < s.size() && s[i] == ':' && digit(s[i + 1])) ++i, number_at(s, &i, &p->col);
    u32 end = 0;
    if (p->col && i + 1 < s.size() && s[i] == '-' && digit(s[i + 1])) {
        ++i, number_at(s, &i, &end);
        if (i + 1 < s.size() && s[i] == ':' && digit(s[i + 1])) ++i, number_at(s, &i, &end);
    }
    p->rest = s.substr(i);
    return true;
}

std::string_view extension(std::string_view path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) return {};
    return path.substr(dot + 1);
}

bool hdl(std::string_view ext) { return ext == "v" || ext == "sv" || ext == "vh" || ext == "svh" || ext == "vhd" || ext == "vhdl" || ext == "ys"; }
bool assembly(std::string_view ext) { return ext == "s" || ext == "S" || ext == "asm"; }
bool python(std::string_view ext) { return ext == "py" || ext == "pyi"; }
bool source(std::string_view ext) {
    static constexpr std::string_view kSource[] = {"c", "cc", "cpp", "cxx", "c++", "C", "m", "mm", "s", "S", "asm", "hip", "cu", "f", "f90", "F90"};
    return std::ranges::find(kSource, ext) != std::end(kSource);
}

// A lint rule's code at the start of s, as Ruff writes them ("F401",
// "PLR0913"), followed by a space or nothing: its length, 0 if none.
size_t rule_code(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && i < 8 && upper(s[i])) ++i;
    if (i == 0) return 0;
    const size_t letters = i;
    while (i < s.size() && i - letters < 5 && digit(s[i])) ++i;
    return i > letters && (i == s.size() || s[i] == ' ') ? i : 0;
}

// A word of lower-case letters, digits and dashes at the start of s
// ("invalid-syntax", "no-tabs", "error"): its length.
size_t kebab(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (lower(s[i]) || (i && (s[i] == '-' || digit(s[i]))))) ++i;
    return i;
}

// A linker's name, as it starts its lines: "ld", "/usr/bin/ld",
// "arm-none-eabi-ld", "ld.bfd", "ld.lld".
bool linker(std::string_view w) {
    if (w.empty() || w.find(' ') != std::string_view::npos) return false;
    w = w.substr(w.rfind('/') + 1);
    return w == "ld" || w.ends_with("-ld") || w.starts_with("ld.") || w == "ld64.lld";
}

// glslang's "'token' : message" (the token often none: "'' :  syntax error").
std::string_view glslang_message(std::string_view m) {
    m = trim(m);
    if (m.starts_with("'' :")) m = trim(m.substr(4));
    return m;
}

void set(BuildProblem& p, std::string_view path, u32 line, u32 col, u8 severity, std::string_view message, std::string_view code) {
    p.path = std::string(path);
    p.line = std::max(line, 1u);
    p.col = col;
    p.severity = severity;
    p.message = std::string(message.substr(0, kMaxJoined));
    p.code = std::string(code);
    p.notes.clear();
}

}  // namespace

bool BuildScan::tool_line(std::string_view text) {
    if (tool_held_ != kHeldNone) {
        if (tool_more(text)) return true;
        tool_flush();
    }
    if (text.empty()) return false;
    tool_command(text);
    // pre-commit's hooks, a line each ("ruff......Failed"), what a failed one
    // printed under it.
    if (text.ends_with("Failed") || text.ends_with("Passed") || text.ends_with("Skipped"))
        if (const size_t dots = text.find("..."); dots != std::string_view::npos && dots > 0) {
            tool_hook_ = text.ends_with("Failed");
            tool_hook_name_ = std::string(trim(text.substr(0, dots)));
            tool_hook_id_.clear();
            return true;
        }
    switch (text[0]) {
        case '%':  // Verilator's: "%Warning-WIDTH: a.v:3:5: message", "%Error: a.v:4:1: syntax error"
            if (text.starts_with("%Error") || text.starts_with("%Warning")) {
                const size_t colon = text.find(": ");
                if (colon == std::string_view::npos) return true;
                std::string_view code;
                if (const size_t dash = text.find('-'); dash < colon) code = text.substr(dash + 1, colon - dash - 1);
                Place p;
                if (!place_at_start(text.substr(colon + 2), &p, false) || !p.rest.starts_with(": ")) return true;  // ("%Error: Exiting due to 2 warning(s)")
                set(tool_problem_, p.path, p.line, p.col, text[1] == 'E' ? 1 : 2, trim(p.rest.substr(2)), code);
                tool_held_ = kHeldVerilator;
                return true;
            }
            break;
        case 'E':
            if (text.starts_with("ERROR: ")) return tool_upper(text.substr(7), 1);
            break;
        case 'W':
            if (text.starts_with("WARNING: ")) return tool_upper(text.substr(9), 2);
            if (text.starts_with("Warning: ") && tool_run_ == kRunNextpnr) return tool_upper(text.substr(9), 2);  // (Yosys's name no place: not taken)
            break;
        case 'F':
            if (text.starts_with("FATAL: ")) return tool_upper(text.substr(7), 1);  // vvp's $fatal
            break;
        case 'C':
            if (text.starts_with("CRITICAL WARNING: [")) return tool_upper(text.substr(18), 2);  // Vivado's
            break;
        case 'S':
            if (text.starts_with("Shader baking failed: ERROR: ")) return tool_upper(text.substr(29), 1);  // qsb's, glslang's lines under it
            break;
        case 'I':
            if (text.starts_with("Info: ")) tool_run_ = kRunNextpnr;  // (nextpnr starts what it says so)
            break;
        case 'U':  // macOS's ld: "Undefined symbols for architecture arm64:", each symbol and its uses under it
            if (text.starts_with("Undefined symbols for architecture ") || text == "Undefined symbols:") {
                tool_problem_ = {};
                tool_held_ = kHeldMacUndefined;
                return true;
            }
            break;
        case 'd':  // and "duplicate symbol '_x' in:", the objects under it
            if (text.starts_with("duplicate symbol ") && text.ends_with(" in:")) {
                set(tool_problem_, "", 1, 0, 1, text.substr(0, text.size() - 4), "");
                tool_held_ = kHeldMacDuplicate;
                return true;
            }
            break;
        case 'p':  // Metal's compiled from source at run time: the file it was is the one a command named, or the project's one .metal file
            if (text.starts_with("program_source:")) {
                if (tool_metal_.empty() && !tool_metal_looked_ && by_name_) {
                    tool_metal_looked_ = true;
                    tool_metal_ = by_name_("*.metal");
                }
                if (!tool_metal_.empty()) return compiler_line(tool_metal_ + std::string(text.substr(14)));
            }
            break;
        case '-':
            if (text.starts_with("-- Running command `") || text.starts_with("-- Executing script file `")) tool_run_ = kRunYosys;
            else if (tool_hook_ && text.starts_with("- hook id: ")) tool_hook_id_ = std::string(trim(text.substr(11)));
            return false;
        case 'e':
        case 'w':  // spirv-val's "error: line 12: message" (the instruction's number in the module), the instruction under it
            if (text.starts_with("error: line ") || text.starts_with("warning: line ")) {
                const u8 sev = text[0] == 'e' ? 1 : 2;
                size_t i = sev == 1 ? 12 : 14;
                u32 n = 0;
                if (number_at(text, &i, &n) && text.substr(i).starts_with(": ")) {
                    std::string_view m = trim(text.substr(i + 2)), code;
                    if (m.starts_with("[VUID-"))
                        if (const size_t close = m.find("] "); close != std::string_view::npos) code = m.substr(1, close - 1), m = m.substr(close + 2);
                    set(tool_problem_, tool_spv_.empty() ? std::string_view("spirv-val") : std::string_view(tool_spv_), n, 0, sev, m, code);
                    tool_held_ = kHeldSpirv;
                    return true;
                }
            }
            break;
        case ' ': {
            const std::string_view t = trim(text);
            // The place under Ruff's and naga's (codespan's) heading: " --> a.py:3:5", "┌─ s.wgsl:2:9".
            const size_t arrow = t.starts_with("--> ") ? 4 : t.starts_with("\xE2\x94\x8C\xE2\x94\x80 ") ? 7 : 0;
            Place q;
            if (arrow && tool_header_at_ + 1 == lines_ && place_at_start(t.substr(arrow), &q)) {
                std::string_view h = tool_header_, code;
                u8 sev = 2;
                if (const size_t n = rule_code(h)) {  // Ruff's "F401 [*] message"
                    code = h.substr(0, n);
                    h = trim(h.substr(n));
                    if (h.starts_with("[*] ")) h.remove_prefix(4);
                } else {  // "invalid-syntax: message", "error: message", "error[E0425]: message"
                    const size_t k = kebab(h);
                    const std::string_view word = h.substr(0, k);
                    h = h.substr(k);
                    if (h.starts_with('['))
                        if (const size_t close = h.find(']'); close != std::string_view::npos) code = h.substr(1, close - 1), h = h.substr(close + 1);
                    if (!h.starts_with(':') || word == "note" || word == "help") return true;  // (a note's own place: not a problem)
                    h = trim(h.substr(1));
                    if (word == "error" || word == "invalid-syntax") sev = 1;
                    if (word != "error" && word != "warning" && code.empty()) code = word;
                    if (h.empty()) h = word;
                }
                set(tool_problem_, q.path, q.line, q.col, sev, h, code);
                tool_held_ = kHeldDiagram;
                return true;
            }
            // pyright's "  /p/a.py:3:5 - error: message (reportX)", its message's other lines under it.
            if (text.starts_with("  ") && place_at_start(t, &q) && q.col && q.rest.starts_with(" - ")) {
                const std::string_view r = q.rest.substr(3);
                const u8 sev = r.starts_with("error: ") ? 1 : r.starts_with("warning: ") ? 2 : 0;
                if (!sev) return r.starts_with("information: ");
                set(tool_problem_, q.path, q.line, q.col, sev, trim(r.substr(sev == 1 ? 7 : 9)), "");
                tool_held_ = kHeldPyright;
                return true;
            }
            if (t.starts_with("|  yosys -- ") || t.starts_with("Yosys ")) tool_run_ = kRunYosys;  // (its banner)
            return false;
        }
        default: break;
    }
    return tool_place(text);
}

// A line that starts with a place, or a program's name: ld's, GNU as's,
// Yosys's, Icarus's, Verible's, Ruff's, tint's, the MSVC form's (DXC's,
// slangc's); and what could head Ruff's or naga's diagram, kept for the
// line under it.
bool BuildScan::tool_place(std::string_view text) {
    if (tool_hook_ && text.starts_with("Fixing ")) {  // pre-commit's hooks that fix files: each it fixed
        if (BuildProblem* p = add(trim(text.substr(7)), 1, 0, 2, "fixed by " + tool_hook_name_)) p->code = tool_hook_id_;
        return true;
    }
    const size_t c = text.find(':');
    if (c != std::string_view::npos) {
        // ld's "/usr/bin/ld: message", and the line under its "in function" (with no name before it).
        if (c + 1 < text.size() && text[c + 1] == ' ' && linker(text.substr(0, c))) return tool_ld(text.substr(c + 2), true);
        if (tool_ld_fn_at_ + 1 == lines_ && tool_ld(text, false)) return true;
        // collect2's and Clang's word that the link failed: a problem only when ld's own lines gave none.
        if (const std::string_view after = text.substr(c); after.starts_with(": error: ld returned ") || after.starts_with(": error: linker command failed")) {
            if (!tool_ld_found_) add("ld", 1, 0, 1, text.substr(c + 9));
            tool_ld_found_ = 0;
            tool_ld_fn_.clear();
            return true;
        }
        Place p;
        if (c + 1 < text.size() && digit(text[c + 1]) && place_at_start(text, &p)) {
            const std::string_view ext = extension(p.path);
            if (p.rest.starts_with(": ")) {
                std::string_view m = p.rest.substr(2), code;
                u8 sev = 0;
                if (hdl(ext) || assembly(ext)) {  // Yosys's "a.v:3: ERROR: ...", GNU as's "a.s:5: Error: ...", and their warnings
                    if (m.starts_with("Error: ") || m.starts_with("ERROR: ")) sev = 1, m.remove_prefix(7);
                    else if (m.starts_with("Warning: ") || m.starts_with("WARNING: ")) sev = 2, m.remove_prefix(9);
                }
                if (!sev && (m == "syntax error" || m.starts_with("sorry: "))) sev = 1;  // Icarus's
                if (!sev && hdl(ext) && p.col) {  // Verible's: "a.sv:4:1-9: syntax error at token ...", "a.sv:2:1: message [Style: tabs] [no-tabs]"
                    if (m.starts_with("syntax error at ") || m.starts_with("preprocessing error at ")) sev = 1;
                    else if (const size_t open = m.rfind(" ["); m.ends_with(']') && open != std::string_view::npos) {
                        code = m.substr(open + 2, m.size() - open - 3);
                        if (!code.empty() && kebab(code) == code.size()) {
                            sev = 2;
                            m = m.substr(0, open);
                            if (const size_t style = m.rfind(" [Style: "); style != std::string_view::npos && m.ends_with(']')) m = m.substr(0, style);
                        } else {
                            code = {};
                        }
                    }
                }
                if (!sev && python(ext)) {  // Ruff's: "a.py:1:8: F401 [*] message", "a.py:1:7: invalid-syntax: message", and its pylint form "a.py:1: [F401] message"
                    if (const size_t n = rule_code(m); n && p.col) {
                        code = m.substr(0, n), m = trim(m.substr(n)), sev = 2;
                        if (m.starts_with("[*] ")) m.remove_prefix(4);
                    } else if (m.starts_with("invalid-syntax: ")) {
                        code = m.substr(0, 14), m.remove_prefix(16), sev = 1;
                    } else if (const size_t close = m.find("] "); m.starts_with('[') && close != std::string_view::npos && rule_code(m.substr(1, close - 1)) == close - 1) {
                        code = m.substr(1, close - 1), m = m.substr(close + 2), sev = 2;
                    }
                }
                if (!sev && tool_hook_ && !m.starts_with("error:") && !m.starts_with("warning:") && !m.starts_with("note:"))  // a failed pre-commit hook's own
                    sev = 1, code = tool_hook_id_;
                if (sev) {
                    if (BuildProblem* added = add(p.path, std::max(p.line, 1u), p.col, sev, trim(m))) added->code = std::string(code);
                    return true;
                }
            } else if (p.col && (p.rest.starts_with(" error: ") || p.rest.starts_with(" warning: "))) {  // tint's "a.wgsl:3:5 error: message", its notes under it
                const u8 sev = p.rest[1] == 'e' ? 1 : 2;
                set(tool_problem_, p.path, p.line, p.col, sev, trim(p.rest.substr(sev == 1 ? 8 : 10)), "");
                tool_held_ = kHeldNotes;
                return true;
            }
        } else if (c > 2 && text[c - 1] == ')' && c + 1 < text.size() && text[c + 1] == ' ') {
            // The MSVC form: DXC's "a.hlsl(12,5): error: ...", FXC's "a.hlsl(12,5-9): error X3000: ...", slangc's "a.slang(12): error 30015: ...".
            const size_t open = text.rfind('(', c);
            size_t i = open + 1;
            u32 line = 0, col = 0, end = 0;
            if (open != std::string_view::npos && open > 0 && number_at(text, &i, &line)) {
                if (i < c && text[i] == ',' && (++i, number_at(text, &i, &col)) && i < c && text[i] == '-') ++i, number_at(text, &i, &end);
                std::string_view m = text.substr(c + 2), code;
                const u8 sev = m.starts_with("error") ? 1 : m.starts_with("fatal error") ? 1 : m.starts_with("warning") ? 2 : 0;
                if (i + 1 == c && sev) {
                    m.remove_prefix(m.starts_with("error") ? 5 : m.starts_with("fatal error") ? 11 : 7);
                    if (const size_t colon = m.find(": "); m.starts_with(' ') && colon != std::string_view::npos && m.substr(1, colon - 1).find(' ') == std::string_view::npos)
                        code = m.substr(1, colon - 1), m = m.substr(colon);
                    if (m.starts_with(": ")) {
                        set(tool_problem_, text.substr(0, open), line, col, sev, trim(m.substr(2)), code);
                        tool_held_ = kHeldNotes;
                        return true;
                    }
                }
            }
        }
    }
    // What could head a diagram whose place is on the next line.
    const bool rule = rule_code(text) != 0;
    if (const size_t k = rule ? 0 : kebab(text); rule || (k && (text.substr(k).starts_with(": ") || text.substr(k).starts_with('[')))) {
        tool_header_.assign(text.substr(0, kMaxJoined));
        tool_header_at_ = lines_;
    }
    return false;
}

// A line after "ERROR: ", "WARNING: ", "FATAL: ": Vivado's "[Synth 8-439]
// message [path:line]", glslang's and vvp's "path:line: message" (qsb's with
// no path: the file its command named), or Yosys's and nextpnr's with no
// place (nextpnr's "(on line 3)" of the .pcf file its command named).
bool BuildScan::tool_upper(std::string_view rest, u8 sev) {
    if (rest.starts_with('[')) {
        const size_t close = rest.find("] ");
        if (close == std::string_view::npos) return false;
        const std::string_view code = rest.substr(1, close - 1);
        std::string_view m = trim(rest.substr(close + 2)), path = "vivado";
        u32 line = 1, col = 0;
        if (const size_t open = m.rfind('['); m.ends_with(']') && open != std::string_view::npos) {
            std::string_view where = m.substr(open + 1, m.size() - open - 2);
            u32 last = 0;
            size_t colon = where.rfind(':'), i = colon + 1;
            if (colon != std::string_view::npos && colon > 0 && number_at(where, &i, &last) && i == where.size()) {
                where = where.substr(0, colon), line = last;
                colon = where.rfind(':'), i = colon + 1;
                if (colon != std::string_view::npos && colon > 0 && number_at(where, &i, &last) && i == where.size()) where = where.substr(0, colon), col = line, line = last;
                path = where;
                m = trim(m.substr(0, open));
            }
        }
        if (BuildProblem* added = add(path, std::max(line, 1u), col, sev, m)) added->code = std::string(code);
        return true;
    }
    if (rest.size() > 1 && rest[0] == ':' && digit(rest[1])) {
        size_t i = 1;
        u32 line = 0;
        number_at(rest, &i, &line);
        if (!rest.substr(i).starts_with(": ")) return false;
        const std::string_view m = glslang_message(rest.substr(i + 2));
        if (m != "compilation terminated") add(tool_qsb_.empty() ? std::string_view("qsb") : std::string_view(tool_qsb_), std::max(line, 1u), 0, sev, m);
        return true;
    }
    Place p;
    if (place_at_start(rest, &p, false) && p.rest.starts_with(": ")) {
        const std::string_view m = glslang_message(p.rest.substr(2));
        if (m != "compilation terminated") add(p.path, std::max(p.line, 1u), p.col, sev, m);  // (what follows an error, not one of its own)
        return true;
    }
    if (tool_run_ == kRunYosys && sev == 1) {
        add("yosys", 1, 0, 1, trim(rest));
        return true;
    }
    if (tool_run_ == kRunNextpnr) {
        std::string_view m = trim(rest), path = "nextpnr";
        u32 line = 1;
        if (const size_t on = m.rfind(" (on line "); !tool_pcf_.empty() && m.ends_with(')') && on != std::string_view::npos) {
            size_t i = on + 10;
            if (number_at(m, &i, &line) && i + 1 == m.size()) path = tool_pcf_, m = m.substr(0, on);
        }
        add(path, std::max(line, 1u), 0, sev, m);
        return true;
    }
    return false;
}

// A linker's line, after its name ("/usr/bin/ld: "), or the line under its
// "in function" (with no name). Its place: "a.o:/src/a.c:12" (from the
// debug information), "a.c:(.text+0x1c)" (the object's source, no line),
// "b.o:(.data+0x0)" (an object, no source); none ("cannot find -lfoo").
// False: not a linker's line.
bool BuildScan::tool_ld(std::string_view body, bool prefixed) {
    u8 sev = 1;
    if (body.starts_with("warning: ")) sev = 2, body.remove_prefix(9);
    else if (body.starts_with("error: ")) body.remove_prefix(7);
    else if (body.starts_with("note: ") || body.starts_with("NOTE: ")) return prefixed;
    std::string_view where, m = body;
    if (const size_t colon = body.find(": "); colon != std::string_view::npos && body.substr(0, colon).find(' ') == std::string_view::npos)
        where = body.substr(0, colon), m = body.substr(colon + 2);
    if (!prefixed && where.find(":(") == std::string_view::npos) return false;
    if (m.starts_with("warning: ")) sev = 2, m.remove_prefix(9);
    if (m.starts_with("in function `") || m.starts_with("In function `")) {
        m.remove_prefix(13);
        tool_ld_fn_ = std::string(m.substr(0, m.find('\'')));
        tool_ld_fn_at_ = lines_;
        return true;
    }
    std::string_view other;  // "...; a.o:/src/a.c:3: first defined here"
    if (const size_t semi = m.find("; "); semi != std::string_view::npos && m.ends_with(": first defined here"))
        other = m.substr(semi + 2, m.size() - semi - 22), m = m.substr(0, semi);
    u32 line = 0;
    std::string path = where.empty() ? std::string() : tool_ld_source(where, &line);
    std::string message(m);
    if (path.empty()) {
        if (!where.empty()) message += " (" + std::string(where.substr(0, where.find(":("))) + ")";
        path = "ld";
    } else if (!line && !tool_ld_fn_.empty()) {
        message += " in `" + tool_ld_fn_ + "'";
    }
    ++tool_ld_found_;
    BuildProblem* added = add(path, std::max(line, 1u), 0, sev, message);
    if (added && !other.empty()) {
        u32 at = 0;
        const std::string first = tool_ld_source(other, &at);
        if (first.empty()) added->notes.push_back({"", 0, 0, "first defined in " + std::string(other.substr(0, other.find(":(")))});
        else added->notes.push_back({resolve(first), std::max(at, 1u), 0, "first defined here"});
    }
    return true;
}

// Where ld says something is ("/tmp/cc1.o:/src/a.c:12", "a.c:(.text+0x1c)",
// "CMakeFiles/app.dir/src/a.c.o", "libx.a(a.o)"): its source file as
// written (empty when it names none: an object not named for one) and its
// line (0: not said).
std::string BuildScan::tool_ld_source(std::string_view where, u32* line) {
    *line = 0;
    if (const size_t section = where.find(":("); section != std::string_view::npos) where = where.substr(0, section);
    if (const size_t colon = where.rfind(':'); colon != std::string_view::npos) {
        size_t i = colon + 1;
        u32 n = 0;
        if (number_at(where, &i, &n) && i == where.size()) *line = n, where = where.substr(0, colon);
    }
    if (const size_t colon = where.rfind(':'); colon != std::string_view::npos) where = where.substr(colon + 1);  // (the object before the source)
    if (where.ends_with(')'))  // an archive's member
        if (const size_t open = where.rfind('('); open != std::string_view::npos) where = where.substr(open + 1, where.size() - open - 2);
    const std::string_view ext = extension(where);
    if (source(ext)) return std::string(where);
    if (ext != "o" && ext != "obj") return {};
    *line = 0;
    const std::string_view stem = where.substr(0, where.size() - ext.size() - 1);  // CMake's objects are named for their sources: src/a.c.o
    if (!source(extension(stem))) return {};
    if (const size_t dir = stem.find(".dir/"); dir != std::string_view::npos) return std::string(stem.substr(dir + 5));  // (CMakeFiles/app.dir/src/a.c.o: the source's path in the project)
    return std::string(stem.substr(stem.rfind('/') + 1));
}

// The line under a problem held: part of it (true), or not (it is added).
bool BuildScan::tool_more(std::string_view text) {
    BuildProblem& p = tool_problem_;
    auto note = [&](std::string_view path, u32 line, u32 col, std::string_view message) {
        if (p.notes.size() < kMaxNotes) p.notes.push_back({path.empty() ? std::string() : resolve(path), line, col, std::string(message.substr(0, kMaxJoined))});
    };
    switch (tool_held_) {
        case kHeldVerilator: {  // "   : ... note: In instance 'top.u0'", "   a.v:2:28: ... Location of other write", its source and carets
            if (text.empty() || text[0] != ' ') return false;
            const std::string_view t = trim(text);
            Place q;
            if (t.starts_with(": ... ")) {
                std::string_view m = t.substr(6);
                if (m.starts_with("note: ")) m.remove_prefix(6);
                note({}, 0, 0, m);
            } else if (place_at_start(t, &q, false) && q.rest.starts_with(": ")) {
                std::string_view m = trim(q.rest.substr(2));
                if (m.starts_with("... ")) m.remove_prefix(4);
                if (!m.empty()) note(q.path, std::max(q.line, 1u), q.col, m);
            }
            return true;
        }
        case kHeldDiagram:  // the diagram under Ruff's or naga's place, naga's "= more" lines in it
            if (text.empty() || (text[0] != ' ' && !digit(text[0]))) return false;
            if (const std::string_view t = trim(text); t.starts_with("= ")) note({}, 0, 0, t.substr(2));
            return true;
        case kHeldPyright:  // its message's other lines, four spaces in
            if (!text.starts_with("    ")) return false;
            if (p.message.size() < kMaxJoined) p.message += "; " + std::string(trim(text).substr(0, kMaxJoined - p.message.size()));
            return true;
        case kHeldNotes: {  // the source and carets under tint's or the MSVC form's problem, and its notes (tint's after a blank line)
            if (text.empty() || text[0] == ' ' || text[0] == '^' || text[0] == '~') return true;
            Place q;
            if (place_at_start(text, &q) && q.col && q.rest.starts_with(" note: ")) {
                note(q.path, q.line, q.col, trim(q.rest.substr(7)));
                return true;
            }
            if (const size_t c = text.find("): note"); c != std::string_view::npos) {
                const size_t open = text.rfind('(', c);
                size_t i = open + 1;
                u32 line = 0, col = 0;
                const size_t colon = text.find(": ", c + 7);
                if (open != std::string_view::npos && open > 0 && number_at(text, &i, &line) && colon != std::string_view::npos) {
                    if (text[i] == ',') ++i, number_at(text, &i, &col);
                    note(text.substr(0, open), line, col, trim(text.substr(colon + 2)));
                    return true;
                }
            }
            return false;
        }
        case kHeldSpirv:  // the instruction, disassembled
            if (text.empty() || text[0] != ' ') return false;
            if (p.notes.empty()) note({}, 0, 0, trim(text));
            return true;
        case kHeldMacUndefined: {  // '  "_helper", referenced from:', '      _main in a.c.o'
            if (text.starts_with("  \"")) {
                tool_flush();
                tool_held_ = kHeldMacUndefined;
                const std::string_view sym = text.substr(3, text.find('"', 3) - 3);
                set(p, "", 1, 0, 1, "undefined symbol: " + std::string(sym), "");
                return true;
            }
            if (text.starts_with("      ") && !p.message.empty()) {
                const std::string_view t = trim(text);
                const size_t in = t.rfind(" in ");
                u32 line = 0;
                const std::string src = in == std::string_view::npos ? std::string() : tool_ld_source(t.substr(in + 4), &line);
                if (p.path.empty()) {
                    p.path = src.empty() ? "ld" : src;
                    p.message += " (referenced from " + std::string(t) + ")";
                } else {
                    note(src, 1, 0, src.empty() ? "also referenced from " + std::string(t) : "also referenced from " + std::string(t.substr(0, in)));
                }
                return true;
            }
            if (text.starts_with("ld: symbol(s) not found")) {
                tool_flush();
                return true;
            }
            return false;
        }
        case kHeldMacDuplicate:  // "    /p/CMakeFiles/app.dir/src/a.c.o", each object with it
            if (text.size() > 4 && text.starts_with("    ") && text[4] != ' ') {
                const std::string_view obj = trim(text);
                u32 line = 0;
                const std::string src = tool_ld_source(obj, &line);
                if (p.path.empty()) {
                    p.path = src.empty() ? "ld" : src;
                    if (src.empty()) p.message += " (in " + std::string(obj) + ")";
                } else {
                    note(src, 1, 0, src.empty() ? "also in " + std::string(obj) : std::string("also defined here"));
                }
                return true;
            }
            if (text.starts_with("ld: ") && text.find(" duplicate symbol") != std::string_view::npos) {
                tool_flush();
                return true;
            }
            return false;
        default: return false;
    }
}

void BuildScan::tool_flush() {
    const u8 held = tool_held_;
    tool_held_ = kHeldNone;
    BuildProblem& p = tool_problem_;
    if (held == kHeldNone || p.message.empty()) return;
    if (held == kHeldPyright && p.message.ends_with(')'))  // its rule, after its message: "(reportUndefinedVariable)"
        if (const size_t open = p.message.rfind(" ("); open != std::string::npos && p.message.compare(open + 2, 6, "report") == 0) {
            p.code = p.message.substr(open + 2, p.message.size() - open - 3);
            p.message.resize(open);
        }
    if (held == kHeldMacUndefined || held == kHeldMacDuplicate) {
        if (p.path.empty()) p.path = "ld";
        ++tool_ld_found_;
    }
    if (BuildProblem* added = add(p.path, p.line, p.col, p.severity, p.message)) {
        added->code = std::move(p.code);
        added->notes = std::move(p.notes);
    }
    p = {};
}

// A command as make echoes it ("qsb -o x.qsb a.frag"), or Ninja shows one
// that failed ("cd /p && /usr/bin/qsb ..."): the files its tool's lines
// leave out, and whose bare "ERROR:" lines are whose.
void BuildScan::tool_command(std::string_view text) {
    size_t sp = text.find(' ');
    if (sp == std::string_view::npos) return;
    std::string_view w = text.substr(0, sp);
    if (w == "cd") {
        const size_t then = text.find(" && ");
        if (then == std::string_view::npos) return;
        text = text.substr(then + 4);
        if ((sp = text.find(' ')) == std::string_view::npos) return;
        w = text.substr(0, sp);
    }
    if (text.ends_with(".metal")) tool_metal_ = std::string(text.substr(text.rfind(' ') + 1));  // (a program given the file it compiles at run time)
    w = w.substr(w.rfind('/') + 1);
    if (w.empty()) return;
    auto last = [&] {  // its last word, not an option
        std::string_view s = trim(text);
        s = s.substr(s.rfind(' ') + 1);
        return s.starts_with('-') ? std::string() : std::string(s);
    };
    switch (w[0]) {
        case 'q':
            if (w == "qsb") tool_qsb_ = last();
            break;
        case 's':
            if (w == "spirv-val") tool_spv_ = last();
            break;
        case 'y':
            if (w == "yosys") tool_run_ = kRunYosys;
            break;
        case 'n':
            if (w.starts_with("nextpnr-")) {
                tool_run_ = kRunNextpnr;
                if (const size_t pcf = text.find(" --pcf "); pcf != std::string_view::npos) {
                    const std::string_view s = text.substr(pcf + 7);
                    tool_pcf_ = std::string(s.substr(0, s.find(' ')));
                }
            }
            break;
        case 'm':
        case 'x':
            if (w == "metal" || w == "xcrun")
                for (std::string_view s = text; !s.empty();) {
                    const size_t b = s.find(' ');
                    const std::string_view word = s.substr(0, b);
                    if (word.ends_with(".metal")) tool_metal_ = std::string(word);
                    s = b == std::string_view::npos ? std::string_view{} : s.substr(b + 1);
                }
            break;
        default: break;
    }
}

void BuildScan::tool_end() {
    tool_flush();
    tool_header_.clear();
}

}  // namespace blit
