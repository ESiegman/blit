#include "buildscan.hpp"

#include <algorithm>

namespace blit {

namespace {

constexpr size_t kMaxMessage = 600;  // bytes of a problem's message kept

bool digit(char c) { return c >= '0' && c <= '9'; }
bool ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '_' || c == '.'; }

std::string_view trim_left(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

std::string_view trim(std::string_view s) {
    s = trim_left(s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

u32 number(std::string_view s) {
    u64 n = 0;
    for (const char c : s) n = std::min<u64>(n * 10 + static_cast<u64>(c - '0'), 0xFFFFFFFFu);
    return static_cast<u32>(n);
}

// "path:line" or "path:line:col" at the end of s: the path, and the numbers.
bool split_location(std::string_view s, std::string_view* path, u32* line, u32* col) {
    size_t e = s.size(), d = e;
    while (d > 0 && digit(s[d - 1])) --d;
    if (d == e || d == 0 || s[d - 1] != ':') return false;
    u32 last = number(s.substr(d, e - d));
    size_t at = d - 1;  // the ':' before the last number
    size_t d0 = at;
    while (d0 > 0 && digit(s[d0 - 1])) --d0;
    if (d0 < at && d0 > 0 && s[d0 - 1] == ':') {  // two numbers: the line and the column
        *line = number(s.substr(d0, at - d0));
        *col = last;
        at = d0 - 1;
    } else {
        *line = last;
        *col = 0;
    }
    *path = trim_left(s.substr(0, at));
    return !path->empty();
}

// A Python name that ends one of its exceptions or warnings ("ValueError",
// "json.decoder.JSONDecodeError", "DeprecationWarning"), at the start of s,
// followed by nothing or ':'. 0: not one; 1 an error; 2 a warning.
u8 python_name(std::string_view s, bool any) {
    size_t n = 0;
    while (n < s.size() && ident(s[n])) ++n;
    if (n == 0 || digit(s[0]) || s[0] == '.' || (n < s.size() && s[n] != ':')) return 0;
    const std::string_view name = s.substr(0, n);
    if (name.ends_with("Warning")) return 2;
    if (any || name.ends_with("Error") || name.ends_with("Exception")) return 1;
    return 0;
}

}  // namespace

std::string tidy_path(std::string_view path) {
    const bool rooted = !path.empty() && path[0] == '/';
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else if (part != ".." || !rooted) parts.push_back(part);
    }
    std::string out = rooted ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out.empty() ? "." : out;
}

BuildScan::BuildScan(std::string dir, std::function<bool(const std::string&)> exists) : exists_(std::move(exists)) {
    if (dir.starts_with("ssh://")) {
        const size_t slash = dir.find('/', 6);
        prefix_ = dir.substr(0, slash);
        dir = slash == std::string::npos ? "/" : dir.substr(slash);
    }
    base_ = tidy_path(dir);
}

// ---------------------------------------------------------------- the bytes

void BuildScan::feed(std::string_view bytes) {
    const char* p = bytes.data();
    const char* const e = p + bytes.size();
    while (p < e) {
        if (esc_ == kText) {
            // A run of plain text (UTF-8 included) goes on the line at once.
            const char* q = p;
            while (q < e && static_cast<u8>(*q) >= 0x20 && *q != 0x7F) ++q;
            if (q > p) {
                if (cr_) line_.clear(), cr_ = false;
                if (line_.size() < kMaxLine) line_.append(p, std::min<size_t>(static_cast<size_t>(q - p), kMaxLine - line_.size()));
                p = q;
                continue;
            }
            const char c = *p++;
            if (c == '\n') {
                cr_ = false;
                line(line_);
                line_.clear();
            } else if (c == '\r') {
                cr_ = true;
            } else if (c == '\x1b') {
                esc_ = kEsc;
            } else if (c == '\t') {
                if (cr_) line_.clear(), cr_ = false;
                if (line_.size() < kMaxLine) line_ += ' ';
            } else if (c == '\b') {
                if (!line_.empty()) line_.pop_back();
            }
            continue;  // (the other controls are nothing on the line)
        }
        const u8 c = static_cast<u8>(*p++);
        switch (esc_) {
            case kEsc:
                esc_ = c == '[' ? kCsi : (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') ? kString : (c >= 0x20 && c <= 0x2F) ? kEscMore : kText;
                break;
            case kEscMore:  // ESC ( B and the like
                if (c >= 0x30) esc_ = kText;
                break;
            case kCsi:
                if (c >= 0x40 && c <= 0x7E) esc_ = kText;
                break;
            case kString:  // OSC (a title, a link) and the like: to BEL or ESC backslash
                if (c == 0x07) esc_ = kText;
                else if (c == 0x1B) esc_ = kStringEsc;
                break;
            case kStringEsc: esc_ = c == '\\' ? kText : kString; break;
            default: esc_ = kText; break;
        }
    }
}

void BuildScan::finish() {
    if (!line_.empty()) line(line_);
    line_.clear();
    cr_ = false;
    esc_ = kText;
    if (cmake_) {
        cmake_ = false;
        add(cmake_problem_.path, cmake_problem_.line, 0, cmake_problem_.severity, cmake_problem_.message);
    }
    py_ = false;
}

// ---------------------------------------------------------------- the lines

void BuildScan::line(std::string_view text) {
    ++lines_;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
    if (cmake_ && cmake_line(text)) return;
    if (python_line(text)) return;
    if (text.starts_with("CMake ")) {
        // "CMake Error at CMakeLists.txt:3 (add_executable):", "CMake Warning
        // (dev) in CMakeLists.txt:", "CMake Deprecation Warning at x.cmake:9 (cmake_minimum_required):"
        size_t at = text.find(" at ");
        const size_t in = text.find(" in ");
        if (at == std::string_view::npos || (in != std::string_view::npos && in < at)) at = in;
        if (at != std::string_view::npos && text.ends_with(':') && at > 6) {
            const std::string_view kind = text.substr(6, at - 6);
            std::string_view where = text.substr(at + 4, text.size() - at - 5);
            if (where.ends_with(')'))
                if (const size_t paren = where.rfind(" ("); paren != std::string_view::npos) where = where.substr(0, paren);
            std::string_view path = where;
            u32 line = 1, col = 0;
            if (!split_location(where, &path, &line, &col)) path = trim(where), line = 1;
            if (!path.empty() && (kind.find("Error") != std::string_view::npos || kind.find("Warning") != std::string_view::npos)) {
                cmake_ = true;
                cmake_problem_.path = std::string(path);
                cmake_problem_.line = std::max(line, 1u);
                cmake_problem_.severity = kind.find("Error") != std::string_view::npos ? 1 : 2;
                cmake_problem_.message.clear();
                return;
            }
        }
    }
    if (text.starts_with("make") || text.starts_with("gmake") || text.starts_with("ninja: ")) {
        if (text.find(": Entering directory ") != std::string_view::npos || text.find(": Leaving directory ") != std::string_view::npos) {
            entered(text);
            return;
        }
    }
    // "make -C sub" as make echoes it: a folder relative paths may be from
    // (macOS's make says nothing when it goes into one).
    if (const size_t c = text.find(" -C "); c != std::string_view::npos && text.substr(0, c).find("make") != std::string_view::npos) {
        std::string_view dir = trim_left(text.substr(c + 4));
        dir = dir.substr(0, dir.find(' '));
        // (From the folder it is in now, or the build's own: whichever has the file.)
        for (const std::string* from : {dirs_.empty() ? &base_ : &dirs_.back(), &base_}) {
            std::string full = dir.starts_with('/') ? tidy_path(dir) : tidy_path(*from + "/" + std::string(dir));
            if (!dir.empty() && hints_.size() < 64 && std::ranges::find(hints_, full) == hints_.end()) {
                hints_.push_back(std::move(full));
                resolved_.clear();
            }
        }
    }
    compiler_line(text);
}

// The lines under CMake's: the message is the first paragraph of them.
// False: the line is not one of them (it is read as itself).
bool BuildScan::cmake_line(std::string_view text) {
    BuildProblem& p = cmake_problem_;
    const std::string_view t = trim(text);
    if (t.empty() || text[0] == ' ') {
        if (t.empty() && (p.message.empty() || p.message.ends_with(':'))) return true;  // (the blank line before it, or before what it says it is about)
        if (!t.empty()) {
            if (p.message.size() < kMaxMessage) {
                if (!p.message.empty()) p.message += ' ';
                p.message.append(t.substr(0, kMaxMessage - std::min(kMaxMessage, p.message.size())));
            }
            return true;
        }
    }
    cmake_ = false;
    if (p.message.empty()) p.message = p.severity == 1 ? "CMake Error" : "CMake Warning";
    add(p.path, p.line, 0, p.severity, p.message);
    return t.empty();
}

// Python's tracebacks: each frame ("  File "path", line 3, in f"), the
// source and carets under it, and the exception last, unindented. False:
// the line is not part of one.
bool BuildScan::python_line(std::string_view text) {
    const std::string_view t = trim_left(text);
    if (t.starts_with("File \"")) {
        const size_t q = t.find('"', 6);
        if (q == std::string_view::npos || !t.substr(q + 1).starts_with(", line ")) return false;
        std::string_view n = t.substr(q + 8);
        size_t d = 0;
        while (d < n.size() && digit(n[d])) ++d;
        if (!d) return false;
        py_ = true;
        const std::string_view path = t.substr(6, q - 6);
        if (!path.starts_with('<')) {  // (not "<frozen runpy>", "<string>": the innermost of the program's own files)
            py_path_ = std::string(path);
            py_line_ = std::max(number(n.substr(0, d)), 1u);
        }
        return true;
    }
    if (text.starts_with("Traceback (most recent call last):")) {
        py_ = true;
        py_path_.clear();
        return true;
    }
    if (!py_) return false;
    if (text.empty() || text[0] == ' ' || text[0] == '\t') return true;  // the source, the carets
    py_ = false;
    if (!python_name(text, true)) return false;
    if (!py_path_.empty()) add(py_path_, py_line_, 0, 1, text);
    py_path_.clear();
    return true;
}

// "path:line:col: error: message" and the like.
bool BuildScan::compiler_line(std::string_view text) {
    size_t from = 0;
    for (;;) {
        const size_t p = text.find(": ", from);
        if (p == std::string_view::npos) return false;
        from = p + 1;
        const std::string_view after = text.substr(p + 2);
        u8 sev = 0;
        size_t skip = 0;
        bool python = false;
        if (after.starts_with("error:")) sev = 1, skip = 6;
        else if (after.starts_with("warning:")) sev = 2, skip = 8;
        else if (after.starts_with("fatal error:")) sev = 1, skip = 12;
        else if (after.starts_with("*** ")) sev = 1, skip = 4;  // make's own
        else if ((sev = python_name(after, false))) python = true;  // Python's warnings, pytest's failures: "x.py:3: DeprecationWarning: ..."
        else continue;
        std::string_view path;
        u32 line = 0, col = 0;
        if (!split_location(text.substr(0, p), &path, &line, &col)) {
            if (python) continue;
            return false;  // ("g++: error: ...", "ld: error: ...": nowhere to go)
        }
        if (python && !path.ends_with(".py")) continue;
        std::string_view message = python ? after : trim(after.substr(skip));
        if (skip == 4 && message.ends_with("Stop.")) message = trim(message.substr(0, message.size() - 5));
        add(path, std::max(line, 1u), col, sev, message);
        return true;
    }
}

// make's and Ninja's "Entering directory 'x'" (and make's "Leaving").
void BuildScan::entered(std::string_view text) {
    size_t at = text.find(": Entering directory ");
    const bool enter = at != std::string_view::npos;
    if (!enter) at = text.find(": Leaving directory ");
    std::string_view dir = text.substr(at + (enter ? 21 : 20));
    if (!dir.empty() && (dir.front() == '\'' || dir.front() == '`' || dir.front() == '"')) dir.remove_prefix(1);
    if (!dir.empty() && (dir.back() == '\'' || dir.back() == '"')) dir.remove_suffix(1);
    resolved_.clear();
    ++dirs_gen_;
    if (!enter) {
        if (!dirs_.empty()) dirs_.pop_back();
        return;
    }
    std::string full = dir.starts_with('/') ? tidy_path(dir) : tidy_path((dirs_.empty() ? base_ : dirs_.back()) + "/" + std::string(dir));
    dirs_.push_back(full);
    full = prefix_ + full;
    std::erase(seen_, full);
    seen_.push_back(std::move(full));
}

std::string BuildScan::resolve(std::string_view path) {
    if (path.starts_with('/')) return prefix_ + tidy_path(path);
    const std::string key(path);
    if (const auto it = resolved_.find(key); it != resolved_.end()) return it->second;
    std::string pick;
    if (exists_) {  // (the folders entered, the latest first, then the build's own, then those make was told to go into)
        for (size_t i = dirs_.size() + 1; i-- > 0 && pick.empty();) {
            std::string full = tidy_path((i ? dirs_[i - 1] : base_) + "/" + key);
            if (exists_(prefix_ + full)) pick = std::move(full);
        }
        for (size_t i = hints_.size(); i-- > 0 && pick.empty();) {
            std::string full = tidy_path(hints_[i] + "/" + key);
            if (exists_(prefix_ + full)) pick = std::move(full);
        }
    }
    if (pick.empty()) pick = tidy_path((dirs_.empty() ? base_ : dirs_.back()) + "/" + key);
    pick = prefix_ + pick;
    if (resolved_.size() < 4096) resolved_.emplace(key, pick);
    return pick;
}

void BuildScan::add(std::string_view path, u32 line, u32 col, u8 severity, std::string_view message) {
    if (count_ >= kMaxProblems) return;
    BuildProblem p;
    p.path = resolve(path);
    p.line = line;
    p.col = col;
    p.severity = severity;
    p.message = std::string(message.substr(0, kMaxMessage));
    std::string key = p.path;
    key += '\0';
    key += std::to_string(line) + ":" + std::to_string(col) + ":" + static_cast<char>('0' + severity);
    key += p.message;
    if (!seen_problems_.insert(std::move(key)).second) return;
    ++count_;
    found_.push_back(std::move(p));
}

// ---------------------------------------------------------------- a place in a line

bool place_in_text(std::string_view text, size_t at, TextPlace* out) {
    // Python's: File "path", line N
    for (size_t f = text.find("File \""); f != std::string_view::npos; f = text.find("File \"", f + 1)) {
        const size_t q = text.find('"', f + 6);
        if (q == std::string_view::npos || !text.substr(q + 1).starts_with(", line ")) continue;
        size_t d = q + 8;
        while (d < text.size() && digit(text[d])) ++d;
        if (d == q + 8 || at < f || at >= d) continue;
        out->path = std::string(text.substr(f + 6, q - f - 6));
        out->line = number(text.substr(q + 8, d - q - 8));
        out->col = 0;
        out->begin = f, out->end = d;
        return !out->path.empty();
    }
    // A word of the line, up to spaces, quotes and brackets: "path:line[:col]" in it.
    auto apart = [](char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '`' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '<' ||
               c == '>' || c == ',' || c == ';' || c == '|';
    };
    if (at >= text.size() || apart(text[at])) return false;
    size_t b = at, e = at;
    while (b > 0 && !apart(text[b - 1])) --b;
    while (e < text.size() && !apart(text[e])) ++e;
    std::string_view word = text.substr(b, e - b);
    // (What ends it that is not part of it: "x.cpp:3:5:", "Makefile:5.")
    while (!word.empty() && (word.back() == ':' || word.back() == '.')) word.remove_suffix(1);
    std::string_view path;
    u32 line = 0, col = 0;
    if (!split_location(word, &path, &line, &col)) return false;
    if (path.find("://") != std::string_view::npos) return false;  // (a URL's port)
    out->path = std::string(path);
    out->line = std::max(line, 1u);
    out->col = col;
    out->begin = b, out->end = b + word.size();
    return true;
}

}  // namespace blit
