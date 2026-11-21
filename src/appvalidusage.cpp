// The Vulkan spec's own statement of a valid usage rule, for a VUID a
// validation layer's message named (buildvalidation.cpp), in a read-only
// tab. It is read from Vulkan-Headers' registry/validusage.json: the
// system's (Linux's vulkan-headers, the Vulkan SDK's, Homebrew's), else a
// copy blit fetches once, at a fixed tag, into its data folder (beside the
// grammars). The file is ~15 MB: it is indexed once, on a thread of its
// own (where each VUID's statement is in it, and its command), and a
// statement read from it when asked for, on that thread too.
#include <SDL3/SDL.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "fsio.hpp"
#include "grammars.hpp"

namespace blit {

namespace {

constexpr const char* kTag = "v1.4.363";  // Vulkan-Headers' tag a fetched copy is from
constexpr size_t kWidth = 80;             // the tab's lines, at most (in characters)

struct Entry {
    u64 at = 0;  // the statement, as the file has it (between its quotes)
    u32 len = 0;
    u32 command = 0, context = 0;  // (in names)
};

// A validusage.json, indexed.
struct Index {
    std::string path, version;
    i64 size = -1, mtime = 0;
    std::vector<std::string> names;
    std::unordered_map<std::string, Entry> entries;
};

struct Statement {
    std::string vuid, command, context, text, version, path;
    std::string why;  // (none found: why not)
};

bool stamp(const std::string& path, i64* size, i64* mtime) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    *size = static_cast<i64>(st.st_size);
    *mtime = static_cast<i64>(st.st_mtime);
    return true;
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string sh_quote(std::string_view s) {
    std::string out = "'";
    for (const char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The copy fetched into the data folder.
std::string fetched_path() { return grammar_dir() + "/vulkan/validusage-" + kTag + ".json"; }

// The validusage.json to read: BLIT_VALIDUSAGE's, the Vulkan SDK's, the
// system's, the newest of ~/VulkanSDK's (LunarG's for macOS), the copy
// fetched; "" if there is none.
std::string find_file() {
    if (const std::string named = env("BLIT_VALIDUSAGE"); !named.empty()) return named;
    std::vector<std::string> tries;
    if (const std::string sdk = env("VULKAN_SDK"); !sdk.empty()) tries.push_back(sdk + "/share/vulkan/registry/validusage.json");
    for (const char* prefix : {"/usr", "/usr/local", "/opt/homebrew"}) tries.push_back(std::string(prefix) + "/share/vulkan/registry/validusage.json");
    if (const std::string home = env("HOME"); !home.empty()) {
        std::vector<FsEntry> sdks = fs_list(home + "/VulkanSDK");
        std::ranges::sort(sdks, [](const FsEntry& a, const FsEntry& b) { return a.name > b.name; });  // (1.4.x after 1.3.x)
        for (const FsEntry& e : sdks)
            if (e.dir) tries.push_back(home + "/VulkanSDK/" + e.name + "/macOS/share/vulkan/registry/validusage.json");
    }
    tries.push_back(fetched_path());
    i64 size = 0, mtime = 0;
    for (const std::string& t : tries)
        if (stamp(t, &size, &mtime)) return t;
    return {};
}

// Where each VUID's statement is in the file: a walk through its JSON
// ({"version info": {...}, "validation": {command: {context: [{"vuid",
// "text", "page"}, ...]}}}), keeping only that.
std::shared_ptr<Index> index_file(const std::string& path, std::string* why) {
    auto ix = std::make_shared<Index>();
    ix->path = path;
    std::string bytes;
    if (!stamp(path, &ix->size, &ix->mtime) || !fs_read(path, &bytes, 256u << 20)) {
        *why = "Cannot read " + path;
        return nullptr;
    }
    const std::string_view s = bytes;
    struct Level {
        bool object;
        std::string_view key;  // (an object's: the member being read)
    };
    std::vector<Level> stack;
    std::unordered_map<std::string_view, u32> named;
    auto name = [&](std::string_view n) {
        const auto [it, fresh] = named.try_emplace(n, static_cast<u32>(ix->names.size()));
        if (fresh) ix->names.emplace_back(n);
        return it->second;
    };
    bool want_key = false;
    std::string_view vuid;
    Entry e;
    bool text = false;
    for (size_t i = 0; i < s.size();) {
        switch (s[i]) {
            case '{':
                stack.push_back({true, {}});
                want_key = true;
                ++i;
                break;
            case '[':
                stack.push_back({false, {}});
                want_key = false;
                ++i;
                break;
            case '}':
                if (stack.size() == 5 && !vuid.empty() && text) {  // (an entry: in "validation", a command, a context, its list)
                    e.command = name(stack[1].key);
                    e.context = name(stack[2].key);
                    ix->entries.emplace(std::string(vuid), e);
                }
                vuid = {};
                text = false;
                if (!stack.empty()) stack.pop_back();
                ++i;
                break;
            case ']':
                if (!stack.empty()) stack.pop_back();
                ++i;
                break;
            case ',':
                want_key = !stack.empty() && stack.back().object;
                ++i;
                break;
            case '"': {
                size_t j = i + 1;
                while (j < s.size() && s[j] != '"') j += s[j] == '\\' ? 2 : 1;
                const std::string_view str = s.substr(i + 1, std::min(j, s.size()) - i - 1);
                if (want_key && !stack.empty() && stack.back().object) {
                    stack.back().key = str;
                    want_key = false;
                } else if (stack.size() == 5 && stack.back().key == "vuid") {
                    vuid = str;
                } else if (stack.size() == 5 && stack.back().key == "text") {
                    e.at = i + 1;
                    e.len = static_cast<u32>(str.size());
                    text = true;
                } else if (stack.size() == 2 && stack[0].key == "version info" && stack[1].key == "api version") {
                    ix->version = std::string(str);
                }
                i = j + 1;
                break;
            }
            default: ++i; break;
        }
    }
    if (ix->entries.empty()) {
        *why = path + " has no valid usage in it";
        return nullptr;
    }
    return ix;
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

// A JSON string's text (between its quotes), its escapes undone.
std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        if (c == 'n' || c == 't') out += ' ';  // (in HTML, space)
        else if (c == 'u' && i + 4 < s.size()) {
            utf8(out, static_cast<u32>(std::strtoul(std::string(s.substr(i + 1, 4)).c_str(), nullptr, 16)));
            i += 4;
        } else if (c != 'r' && c != 'b' && c != 'f') out += c;
    }
    return out;
}

// The statement's HTML as text: its tags gone (a list's items on lines of
// their own, after a bullet; paragraphs on lines of their own), its
// entities their characters, its spaces one.
std::string plain(std::string_view html) {
    std::string out;
    bool fresh = true;  // (nothing on this line yet but its bullet)
    auto newline = [&] {
        if (!fresh) out += '\n';
        fresh = true;
    };
    for (size_t i = 0; i < html.size(); ++i) {
        const char c = html[i];
        if (c == '<') {
            const size_t e = html.find('>', i);
            if (e == std::string_view::npos) break;
            std::string_view tag = html.substr(i + 1, e - i - 1);
            const bool closing = tag.starts_with('/');
            if (closing) tag.remove_prefix(1);
            tag = tag.substr(0, tag.find(' '));
            if (tag == "li" && !closing) {
                newline();
                out += "  \xE2\x80\xA2 ";
            } else if (tag == "p" || tag == "div" || tag == "ul" || tag == "li" || tag == "br") {
                newline();
            }
            i = e;
            continue;
        }
        if (c == '&') {
            const size_t e = html.find(';', i);
            if (e != std::string_view::npos && e - i <= 8) {
                const std::string_view ent = html.substr(i + 1, e - i - 1);
                std::string ch;
                if (ent == "gt") ch = ">";
                else if (ent == "lt") ch = "<";
                else if (ent == "amp") ch = "&";
                else if (ent == "quot") ch = "\"";
                else if (ent.starts_with('#')) utf8(ch, static_cast<u32>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10)));
                if (!ch.empty() && ch != "\xE2\x80\x8B") {  // (a zero-width space: nothing)
                    out += ch;
                    fresh = false;
                }
                if (!ch.empty() || ent.starts_with('#')) {
                    i = e;
                    continue;
                }
            }
        }
        if (c == ' ' || c == '\n') {
            if (!fresh && out.back() != ' ') out += ' ';
            continue;
        }
        out += c;
        fresh = false;
    }
    // (No spaces at the ends of lines, nor a line after the last.)
    std::string tidy;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == ' ' && (i + 1 == out.size() || out[i + 1] == '\n')) continue;
        tidy += out[i];
    }
    while (!tidy.empty() && tidy.back() == '\n') tidy.pop_back();
    return tidy;
}

// Each line of text in lines of `width` characters at most, broken at
// spaces (a list item's go on under its words, not its bullet).
std::string wrapped(const std::string& text, size_t width) {
    std::string out;
    size_t from = 0;
    while (from <= text.size()) {
        const size_t nl = text.find('\n', from);
        const std::string_view line = std::string_view(text).substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        from = nl == std::string::npos ? text.size() + 1 : nl + 1;
        const size_t indent = line.starts_with("  \xE2\x80\xA2 ") ? 4 : 0, lead = indent ? 6 : 0;  // (the bullet: in characters, in bytes)
        auto chars = [](std::string_view s) {
            size_t n = 0;
            for (const char c : s) n += (static_cast<u8>(c) & 0xC0) != 0x80;
            return n;
        };
        std::string_view rest = line;
        bool first = true;
        while (!rest.empty()) {
            const size_t room = width - (first ? 0 : indent);
            size_t cut = rest.size();
            if (chars(rest) > room) {
                size_t at = 0, n = 0;
                for (; at < rest.size() && n < room; ++at) n += (static_cast<u8>(rest[at]) & 0xC0) != 0x80;
                const size_t space = rest.rfind(' ', at);
                cut = space != std::string_view::npos && space > (first ? lead : 0) ? space : at;
            }
            if (!first) out.append(indent, ' ');
            out.append(rest.substr(0, cut));
            out += '\n';
            rest = rest.substr(cut);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            first = false;
        }
        if (line.empty()) out += '\n';
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 40;
    SDL_PushEvent(&ev);
}

// The thread's work: the file found (fetched, if there is none), indexed
// if it has not been (or has changed: index is kept, under mu), the
// statement read from it.
Statement read_statement(std::mutex& mu, std::shared_ptr<Index>& index, const std::string& vuid, bool fetch) {
    Statement st;
    st.vuid = vuid;
    std::string path = find_file();
    if (path.empty() && fetch) {
        const std::string to = fetched_path(), dir = to.substr(0, to.rfind('/'));
        std::string err, said;
        fs_mkdirs(dir, &err);
        const std::string url = std::string("https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/") + kTag + "/registry/validusage.json";
        fs_run(dir, "curl -fsSL --proto =https --max-time 300 -o " + sh_quote(to + ".part") + " " + sh_quote(url) + " && mv " + sh_quote(to + ".part") + " " + sh_quote(to), &said,
               310000);
        i64 size = 0, mtime = 0;
        if (stamp(to, &size, &mtime)) path = to;
        else {
            while (!said.empty() && (said.back() == '\n' || said.back() == ' ')) said.pop_back();
            st.why = "No validusage.json here, and fetching Vulkan-Headers' failed" + (said.empty() ? std::string() : ": " + said.substr(said.rfind('\n') + 1));
            return st;
        }
    }
    if (path.empty()) {
        st.why = "No validusage.json here";
        return st;
    }
    std::shared_ptr<Index> ix;
    {
        std::lock_guard lock(mu);
        ix = index;
    }
    i64 size = 0, mtime = 0;
    if (!ix || ix->path != path || !stamp(path, &size, &mtime) || size != ix->size || mtime != ix->mtime) {
        ix = index_file(path, &st.why);
        if (!ix) return st;
        std::lock_guard lock(mu);
        index = ix;
    }
    st.path = path;
    st.version = ix->version;
    const auto it = ix->entries.find(vuid);
    if (it == ix->entries.end()) {
        st.why = vuid + " is not in " + path + (ix->version.empty() ? "" : " (Vulkan " + ix->version + ")");
        return st;
    }
    std::string raw(it->second.len, '\0');
    FILE* f = std::fopen(path.c_str(), "rb");
    const bool read = f && std::fseek(f, static_cast<long>(it->second.at), SEEK_SET) == 0 && std::fread(raw.data(), 1, raw.size(), f) == raw.size();
    if (f) std::fclose(f);
    if (!read) {
        st.why = "Cannot read " + path;
        return st;
    }
    st.command = ix->names[it->second.command];
    st.context = ix->names[it->second.context];
    st.text = plain(unescape(raw));
    return st;
}

}  // namespace

// The thread's state: kept by it as well (a fetch takes as long as it
// takes; quitting does not wait for it).
struct App::ValidUsage {
    std::mutex mu;
    std::shared_ptr<Index> index;     // (mu) the file indexed
    std::optional<Statement> done;    // (mu) what the thread found
    bool busy = false;                // (mu)
    u64 tab = 0;                      // the tab statements show in
};

void App::valid_usage_show(const std::string& vuid) {
    if (!validusage_) validusage_ = std::make_shared<ValidUsage>();
    std::shared_ptr<ValidUsage> vu = validusage_;
    {
        std::lock_guard lock(vu->mu);
        if (vu->busy) return notify("Still reading validusage.json");
        vu->busy = true;
        if (!vu->index && find_file().empty() && env("BLIT_VALIDUSAGE").empty()) notify(std::string("Fetching validusage.json from Vulkan-Headers ") + kTag + "\xE2\x80\xA6");
    }
    const bool fetch = env("BLIT_VALIDUSAGE").empty();
    std::thread([vu, vuid, fetch] {
        Statement st = read_statement(vu->mu, vu->index, vuid, fetch);
        {
            std::lock_guard lock(vu->mu);
            vu->done = std::move(st);
            vu->busy = false;
        }
        wake();
    }).detach();
}

// What the thread read, in the tab (made beside the one in front, or the
// one there is): the VUID, its command, the statement, and where it is from.
void App::valid_usage_sync() {
    if (!validusage_) return;
    ValidUsage& vu = *validusage_;
    std::optional<Statement> got;
    {
        std::lock_guard lock(vu.mu);
        got.swap(vu.done);
    }
    if (!got) return;
    const Statement& st = *got;
    if (!st.why.empty()) return notify(st.why, 1);
    std::string text;
    std::vector<u32> off;
    std::vector<u8> style;
    auto add = [&](const std::string& s, u8 ink) {
        off.push_back(static_cast<u32>(text.size()));
        style.push_back(ink);
        text += s;
    };
    add(st.vuid, 3);
    add("\n" + st.command + (st.context.empty() || st.context == "core" ? "" : " " + st.context) + "\n\n", 0);
    add(wrapped(st.text, kWidth) + "\n\n", 0);
    add(std::string(st.version.empty() ? "" : "Vulkan " + st.version + ", ") + st.path, 4);
    Doc* d = debug_tab(vu.tab, st.vuid.c_str());
    d->title = st.vuid;
    debug_tab_fill(*d, std::move(text), std::move(off), std::move(style), 0, 0);
}

// Enter on the build's problem i: its statement, for one a VUID names and
// no line of a file (one with a place, as a shader's, goes there).
bool App::valid_usage_row(size_t i) {
    const std::vector<BuildProblem>& bp = build_problems();
    if (i >= bp.size() || !bp[i].path.empty() || !bp[i].code.starts_with("VUID-")) return false;
    panel_problem_seen(i);
    valid_usage_show(bp[i].code);
    return true;
}

void App::valid_usage_picked() {
    const std::vector<BuildProblem>& bp = build_problems();
    const size_t i = panel_problem();
    if (i >= bp.size() || !bp[i].code.starts_with("VUID-")) return notify("The problem picked names no VUID");
    valid_usage_show(bp[i].code);
}

// (Tests.) The thread done (30 s at most), and what it read shown.
void App::valid_usage_wait() {
    for (int i = 0; i < 3000 && validusage_; ++i) {
        {
            std::lock_guard lock(validusage_->mu);
            if (!validusage_->busy) break;
        }
        SDL_Delay(10);
    }
    valid_usage_sync();
}

}  // namespace blit
