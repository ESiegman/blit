// Part of the build's output read for problems (buildscan.hpp): Vulkan's validation layers' messages (text and JSON) and Metal's API validation, as problems with their VUID as the code.
//
// The Khronos validation layer prints, with no messenger of the program's
// own (or a messenger prints its pMessage, after words of its own):
//
//   Validation Error: [ VUID-VkBufferCreateInfo-size-00912 ] | MessageID = 0x7c54445e
//   vkCreateBuffer(): pCreateInfo->size is zero.
//   The Vulkan spec states: size must be greater than 0 (https://docs.vulkan.org/...)
//   Objects: 1
//       [0] VkCommandBuffer 0x55add6df8990[frame commands]
//
// and a blank line; before 1.3.290 all of it on the first line ("... ]
// Object 0: handle = 0x1, type = VK_OBJECT_TYPE_DEVICE; | MessageID = 0x2 |
// vkF(): what. The Vulkan spec states: ..."). Told to (message_format_json,
// VK_LAYER_MESSAGE_FORMAT_JSON=1) it prints an object, a member a line:
// "Severity", "VUID", "Objects", "Function", "Location", "MainMessage", ...
// A Vulkan message has no place in a file (the layer knows the call, not
// where in the program it was made): its problem has none, unless
// GPU-assisted validation names the shader's own line ("Shader validation
// error occurred at oob.comp:7", with the shader built with debug info).
//
// Metal's API validation (MTL_DEBUG_LAYER=1) fails an assertion,
// "-[MTLDebugRenderCommandEncoder validateCommonDrawErrors:...]:5964: failed
// assertion `Draw Errors Validation" with what is wrong on the lines under
// it, to a line ending "'"; told to log instead
// (MTL_DEBUG_LAYER_ERROR_MODE=nslog) the heading is NSLog's line, with
// the one thing wrong under it. Its shader validation (MTL_SHADER_VALIDATION=1, with
// MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1) reports between lines of "=",
// "Invalid Metal usage: ...", at the first frame of its GPU stack.
//
// The same message again (every frame, about another frame's objects: its
// lines the same but for their handles) is the row it made, counted.
#include "buildscan.hpp"

#include <algorithm>

namespace blit {

namespace {

constexpr size_t kMaxText = 600;  // bytes of a message kept (as buildscan.cpp's)
constexpr size_t kMaxNotes = 16;  // lines of a message's own kept, after its first

enum : u8 { kNone, kVkText, kVkJson, kVkJsonMaybe, kMtlAssert, kMtlLog, kMtlReport, kMtlReportMaybe };

// What a line inside a message is: more of it; its last line; or not its
// (it ends before it, and the line is read as itself).
enum Step : u8 { kTaken, kEndTaken, kEndNot };

bool digit(char c) { return c >= '0' && c <= '9'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

u32 number(std::string_view s) {
    u64 n = 0;
    for (const char c : s) n = std::min<u64>(n * 10 + static_cast<u64>(c - '0'), 0xFFFFFFFFu);
    return static_cast<u32>(n);
}

// "path:line" or "path:line:col", all of s.
bool place_of(std::string_view s, std::string_view* path, u32* line, u32* col) {
    u32 nums[2] = {};
    int n = 0;
    while (n < 2) {
        size_t d = s.size();
        while (d > 0 && digit(s[d - 1])) --d;
        if (d == s.size() || d == 0 || s[d - 1] != ':') break;
        nums[n++] = number(s.substr(d));
        s = s.substr(0, d - 1);
    }
    if (n == 0 || s.empty()) return false;
    *path = s;
    *line = std::max(n == 2 ? nums[1] : nums[0], 1u);
    *col = n == 2 ? nums[0] : 0;
    return true;
}

void note(BuildProblem& m, std::string_view text, std::string_view path = {}, u32 line = 0, u32 col = 0) {
    if (m.notes.size() >= kMaxNotes || text.empty()) return;
    m.notes.push_back({std::string(path), line, col, std::string(text.substr(0, kMaxText))});
}

void say(std::string& to, std::string_view more, std::string_view between = " ") {
    if (to.size() >= kMaxText || more.empty()) return;
    if (!to.empty()) to += between;
    to.append(more.substr(0, kMaxText - std::min(kMaxText, to.size())));
}

// A Vulkan message's severity, as the layer words it: 1 an error, 2 a
// warning, 0 one not shown (Information, Verbose), -1 none of them.
int vk_severity(std::string_view word) {
    if (word == "Error") return 1;
    if (word == "Warning" || word == "Performance Warning") return 2;
    if (word == "Information" || word == "Verbose") return 0;
    return -1;
}

// "Validation Error: [ VUID-x ] ...", anywhere in the line: its severity,
// its code, and what follows the code.
bool vk_header(std::string_view text, int* sev, std::string_view* code, std::string_view* rest) {
    for (size_t at = text.find("Validation "); at != std::string_view::npos; at = text.find("Validation ", at + 1)) {
        std::string_view t = text.substr(at + 11);
        const size_t colon = t.find(": [ ");
        if (colon == std::string_view::npos || colon > 20) continue;
        const int s = vk_severity(t.substr(0, colon));
        const size_t close = t.find(" ]", colon + 4);
        if (s < 0 || close == std::string_view::npos) continue;
        *sev = s;
        *code = t.substr(colon + 4, close - colon - 4);
        *rest = t.substr(close + 2);
        return true;
    }
    return false;
}

// GPU-assisted validation's place in the shader's own source: the
// problem goes there (the first such place it names).
bool shader_place(std::string_view line, BuildProblem& m) {
    static constexpr std::string_view kAt[] = {"Shader validation error occurred at ", "Debug shader printf message generated at "};
    for (const std::string_view head : kAt) {
        const size_t a = line.find(head);
        if (a == std::string_view::npos) continue;
        std::string_view path;
        u32 l = 0, c = 0;
        if (place_of(trim(line.substr(a + head.size())), &path, &l, &c) && m.path.empty()) {
            m.path = std::string(path);
            m.line = l;
            m.col = c;
        }
        return true;
    }
    return false;
}

// The text form's lines after its first: the message (its first line, the
// application's name before it put aside), what more it says (notes, and
// GPU-assisted validation's place), the spec's statement (which Enter
// shows in full from validusage.json), the objects it names; a blank line
// ends it. tail: past the statement, where only the objects go on.
Step vk_text_body(std::string_view text, BuildProblem& m, bool& said, bool& tail, bool& more) {
    const std::string_view t = trim(text);
    if (t.empty()) {
        if (!more) return kEndTaken;
        more = false;  // (the blank line between GPU-assisted validation's place and its source line)
        return kTaken;
    }
    more = false;
    if (t.starts_with("Objects: ")) return tail = true, kTaken;
    if (tail) {
        if (!t.starts_with('[') || !text.starts_with(' ')) return kEndNot;
        const size_t e = t.find("] ");
        note(m, e == std::string_view::npos ? t : t.substr(e + 2));
        return kTaken;
    }
    if (t.starts_with("The Vulkan spec states: ")) return tail = true, kTaken;
    if (!said) {
        said = true;
        std::string_view msg = t;
        if (msg.starts_with("[AppName: "))
            if (const size_t e = msg.find("] "); e != std::string_view::npos) msg = msg.substr(e + 2);
        m.message.clear();
        say(m.message, msg);
        return kTaken;
    }
    if (shader_place(t, m)) return more = true, kTaken;
    note(m, t);
    return kTaken;
}

void utf8(std::string& out, u32 c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The JSON string at s[*at] (a '"'), its escapes undone; *at goes past it.
std::string json_string(std::string_view s, size_t* at) {
    std::string out;
    size_t i = *at + 1;
    for (; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (const char c = s[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': case 'b': case 'f': break;
            case 'u': {
                u32 v = 0;
                size_t k = 0;
                for (; k < 4 && i + 1 < s.size(); ++k) {
                    const char h = s[i + 1];
                    const int d = digit(h) ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0) break;
                    v = v * 16 + static_cast<u32>(d);
                    ++i;
                }
                if (k) utf8(out, v);
                break;
            }
            default: out += c; break;
        }
    }
    *at = std::min(i + 1, s.size());
    return out;
}

// In `line`, the string after the member "key" (as the layer writes an
// object: {"type" : "VkQueue", "handle" : "0x1", "name" : ""}).
std::string json_member(std::string_view line, std::string_view key) {
    const size_t k = line.find(key);
    if (k == std::string_view::npos) return {};
    size_t at = line.find('"', k + key.size());
    return at == std::string_view::npos ? std::string() : json_string(line, &at);
}

// The JSON form's lines after its "{": a member each, the objects a line
// each; "}" ends it. objects: inside "Objects" : [ ... ].
Step vk_json_body(std::string_view text, BuildProblem& m, bool& show, bool& objects) {
    const std::string_view t = trim(text);
    if (objects) {
        if (t.starts_with(']')) return objects = false, kTaken;
        std::string o = json_member(t, "\"type\"") + " " + json_member(t, "\"handle\"");
        if (const std::string name = json_member(t, "\"name\""); !name.empty()) o += "[" + name + "]";
        note(m, o);
        return kTaken;
    }
    if (t == "}" || t == "},") return kEndTaken;
    if (!t.starts_with('"')) return kTaken;
    size_t at = 0;
    const std::string key = json_string(t, &at);
    while (at < t.size() && (t[at] == ' ' || t[at] == ':')) ++at;
    if (at < t.size() && t[at] == '[') {
        if (key == "Objects" && !t.substr(at).starts_with("[]")) objects = true;
        return kTaken;
    }
    if (at >= t.size() || t[at] != '"') return kTaken;
    const std::string v = json_string(t, &at);
    if (key == "Severity") {
        const int s = vk_severity(v);
        show = s > 0;
        m.severity = s > 0 ? static_cast<u8>(s) : 1;
    } else if (key == "VUID") {
        m.code = v;
    } else if (key == "Function") {
        m.message.clear();
        if (!v.empty()) say(m.message, v + "():");
    } else if (key == "Location") {
        say(m.message, v);
    } else if (key == "MainMessage") {  // (its first line goes on the message; the others are notes)
        std::string_view rest = v;
        for (bool first = true; !rest.empty(); first = false) {
            const size_t nl = rest.find('\n');
            const std::string_view l = trim(rest.substr(0, nl));
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (first) say(m.message, l);
            else if (!l.empty() && !shader_place(l, m)) note(m, l);
        }
    } else if (key == "DebugRegion" && !v.empty()) {
        note(m, "Debug region: " + v);
    }
    return kTaken;
}

// "2026-11-21 12:30:33.178 bad[80870:13486131] ": NSLog's start of a line;
// what follows it (0: not one).
size_t nslog_stamp(std::string_view text) {
    if (text.size() < 26 || !digit(text[0]) || text[4] != '-' || text[10] != ' ' || text[13] != ':') return 0;
    const size_t e = text.find("] ", 24);
    return e == std::string_view::npos ? 0 : e + 2;
}

// A line under Metal's heading: the first goes on the message, the others are notes.
void metal_body(std::string_view text, BuildProblem& m, bool& said) {
    const std::string_view t = trim(text);
    if (t.empty()) return;
    if (!said) say(m.message, t, ": ");
    else note(m, t);
    said = true;
}

// A message with its numbers in hexadecimal (handles, addresses) taken
// out: the same message about another frame's objects is the same.
std::string without_handles(std::string_view m) {
    std::string out;
    out.reserve(m.size());
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i] == '0' && i + 2 < m.size() && (m[i + 1] == 'x' || m[i + 1] == 'X')) {
            size_t e = i + 2;
            while (e < m.size() && (digit(m[e]) || (m[e] >= 'a' && m[e] <= 'f') || (m[e] >= 'A' && m[e] <= 'F'))) ++e;
            if (e > i + 2) {
                out += '#';
                i = e - 1;
                continue;
            }
        }
        out += m[i];
    }
    return out;
}

}  // namespace

bool BuildScan::validation_line(std::string_view text) {
    auto begin = [&](u8 kind, int sev, std::string_view code, std::string_view message) {
        val_ = kind;
        val_problem_ = {};
        val_problem_.severity = sev == 2 ? 2 : 1;
        val_problem_.code = std::string(code.substr(0, kMaxText));
        say(val_problem_.message, message);
        val_show_ = sev > 0;
        val_said_ = val_spec_ = val_more_ = false;
    };
    if (val_ != kNone) {
        Step step = kEndNot;
        int sev = 0;
        std::string_view code, rest;
        switch (val_) {
            case kVkText:
                if (text.find("Validation ") == std::string_view::npos || !vk_header(text, &sev, &code, &rest))  // (else the next message: this one has ended)
                    step = vk_text_body(text, val_problem_, val_said_, val_spec_, val_more_);
                break;
            case kVkJson: step = vk_json_body(text, val_problem_, val_show_, val_more_); break;
            case kVkJsonMaybe:  // (a line ending "{": the layer's JSON if this is its first member)
                if (const std::string_view t = trim(text); t.starts_with("\"Severity\"") || t.starts_with("\"AppName\"")) {
                    begin(kVkJson, 1, {}, {});
                    vk_json_body(text, val_problem_, val_show_, val_more_);
                    return true;
                }
                break;
            case kMtlAssert:  // (to a line that ends "'")
                if (text.ends_with('\'')) {
                    metal_body(text.substr(0, text.size() - 1), val_problem_, val_said_);
                    step = kEndTaken;
                } else {
                    metal_body(text, val_problem_, val_said_);
                    step = kTaken;
                }
                break;
            case kMtlLog:  // (the line under it: NSLog's message is one thing wrong; what comes next is not known to be its)
                if (!trim(text).empty() && !nslog_stamp(text)) {
                    metal_body(text, val_problem_, val_said_);
                    step = kEndTaken;
                }
                break;
            case kMtlReportMaybe:  // (a line of "=": Metal's shader validation's report if this is its first line)
                if (text.starts_with("Invalid Metal usage: ")) {
                    begin(kMtlReport, 1, {}, trim(text.substr(21)));
                    return true;
                }
                break;
            case kMtlReport: {  // (to the line of "=" under it: the first frame of its GPU stack is its place)
                const std::string_view t = trim(text);
                step = kTaken;
                if (t.size() >= 20 && t.find_first_not_of('=') == std::string_view::npos) {
                    step = kEndTaken;
                } else if (const size_t f = t.find("frame #"); f != std::string_view::npos && f <= 2) {
                    std::string_view path;
                    u32 l = 0, c = 0;
                    const size_t dash = t.rfind(" - ");
                    if (dash != std::string_view::npos && place_of(t.substr(dash + 3), &path, &l, &c)) {
                        if (val_problem_.path.empty()) val_problem_.path = std::string(path), val_problem_.line = l, val_problem_.col = c;
                        else note(val_problem_, t.substr(f), path, l, c);
                    }
                } else if (t != "GPU stack trace:") {
                    note(val_problem_, t);
                }
                break;
            }
            default: break;
        }
        if (step == kTaken) return true;
        validation_end();
        if (step == kEndTaken) return true;
    }
    // A line that begins one: the cost of one that does not is a few
    // compares and a look for 'V' and '`'.
    if (text.empty()) return false;
    if (text.back() == '{') {  // (the layer's JSON, if the next line is its first member: until then the line is read as itself)
        val_ = kVkJsonMaybe;
        return false;
    }
    if (text[0] == '=' && text.size() >= 20 && text.find_first_not_of('=') == std::string_view::npos) {
        val_ = kMtlReportMaybe;
        return false;
    }
    int sev = 0;
    std::string_view code, rest;
    if (vk_header(text, &sev, &code, &rest)) {
        begin(kVkText, sev, code, {});
        // All of it on this line (the layer before 1.3.290): the objects,
        // the message after the MessageID, up to the spec's statement.
        if (const size_t id = rest.find("| MessageID = "); id != std::string_view::npos) {
            const std::string_view after = rest.substr(id + 14);
            if (const size_t bar = after.find(" | "); bar != std::string_view::npos) {
                std::string_view objects = trim(rest.substr(0, id));
                while (!objects.empty()) {
                    const size_t semi = objects.find(';');
                    std::string_view o = trim(objects.substr(0, semi));
                    objects = semi == std::string_view::npos ? std::string_view{} : objects.substr(semi + 1);
                    if (o.starts_with("Object "))
                        if (const size_t c = o.find(": "); c != std::string_view::npos) o = o.substr(c + 2);
                    note(val_problem_, o);
                }
                std::string_view msg = after.substr(bar + 3);
                if (const size_t s = msg.find(" The Vulkan spec states: "); s != std::string_view::npos) msg = msg.substr(0, s);
                say(val_problem_.message, trim(msg));
                validation_end();
            }
        }
        return true;
    }
    if (text.ends_with(" Validation")) {  // Metal's heading, logged by NSLog
        if (const size_t at = nslog_stamp(text)) {
            const std::string_view heading = text.substr(at);
            begin(kMtlLog, 1, heading, heading);
            return true;
        }
    }
    if (const size_t q = text.find('`'); q != std::string_view::npos && q >= 17 && text.substr(q - 17, 17) == "failed assertion " &&
                                          text.substr(0, q).find("MTL") != std::string_view::npos) {  // Metal's assertion
        std::string_view heading = text.substr(q + 1);
        const bool whole = heading.ends_with('\'');
        if (whole) heading.remove_suffix(1);
        begin(kMtlAssert, 1, heading, heading);
        if (whole) validation_end();
        return true;
    }
    return false;
}

// The message being read is added (a problem with no place, as the layer
// gives none, unless it named the shader's line), or, said before, counted.
void BuildScan::validation_end() {
    const u8 was = val_;
    val_ = kNone;
    if (was == kNone || was == kVkJsonMaybe || was == kMtlReportMaybe) return;
    BuildProblem m = std::move(val_problem_);
    val_problem_ = {};
    if (!val_show_) return;
    if (m.message.empty()) m.message = m.code;
    std::string key(1, static_cast<char>('0' + m.severity));
    key += m.code;
    key += '\0';
    key += m.path + ":" + std::to_string(m.line) + ":" + std::to_string(m.col);
    key += '\0';
    key += without_handles(m.message);
    for (const BuildNote& n : m.notes) key += '\0' + without_handles(n.message);
    if (const auto it = val_seen_.find(key); it != val_seen_.end()) {
        BuildProblem& first = it->second;
        ++first.count;
        // (On the row, if it has not been taken yet: else a word that says
        // how many times now, which build_found counts on the row taken.)
        for (size_t i = found_.size(), k = 0; i-- > 0 && k < 8; ++k) {
            BuildProblem& f = found_[i];
            if (f.severity == first.severity && f.line == first.line && f.col == first.col && f.code == first.code && f.message == first.message && f.path == first.path) {
                f.count = first.count;
                return;
            }
        }
        found_.push_back(first);
        return;
    }
    if (val_seen_.size() >= kMaxProblems) return;
    BuildProblem* p = nullptr;
    if (!m.path.empty()) {
        p = add(m.path, m.line, m.col, m.severity, m.message);
    } else if (count_ < kMaxProblems) {
        ++count_;
        p = &found_.emplace_back();
        p->severity = m.severity;
        p->message = std::move(m.message);
    }
    if (!p) return;
    p->code = std::move(m.code);
    p->notes = std::move(m.notes);
    BuildProblem first = *p;
    first.notes.clear();
    val_seen_.emplace(std::move(key), std::move(first));
}

}  // namespace blit
