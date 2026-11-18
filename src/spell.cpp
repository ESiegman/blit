#include "spell.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if !defined(__APPLE__) && !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace blit {

std::vector<std::pair<u32, u32>> spell_words(std::string_view t) {
    std::vector<std::pair<u32, u32>> out;
    auto letter = [](unsigned char c) { return std::isalpha(c) || c >= 0x80; };
    size_t i = 0;
    while (i < t.size()) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (!letter(c) && !std::isdigit(c) && c != '_') {
            ++i;
            continue;
        }
        // A run of what names are made of; it counts only if it is a word.
        size_t e = i;
        bool code = false, high = false;
        while (e < t.size()) {
            const unsigned char d = static_cast<unsigned char>(t[e]);
            if (letter(d)) {
                if (d >= 0x80) high = true;
                if (std::isupper(d) && e > i) code = true;
                ++e;
            } else if (std::isdigit(d) || d == '_') {
                code = true;
                ++e;
            } else if (d == '\'' && e + 1 < t.size() && letter(static_cast<unsigned char>(t[e + 1])) && e > i) {
                ++e;
            } else {
                break;
            }
        }
        const bool joined = (i > 0 && (t[i - 1] == '.' || t[i - 1] == '/' || t[i - 1] == '\\' || t[i - 1] == '@' || t[i - 1] == ':' || t[i - 1] == '-' || t[i - 1] == '#' || t[i - 1] == '$')) ||
                            (e < t.size() && (t[e] == '/' || t[e] == '\\' || t[e] == '@' || t[e] == '(' || (t[e] == '.' && e + 1 < t.size() && std::isalnum(static_cast<unsigned char>(t[e + 1])))));
        bool caps = true;
        for (size_t k = i; k < e; ++k) caps = caps && !std::islower(static_cast<unsigned char>(t[k]));
        if (!code && !high && !joined && !caps && e - i >= 3) out.emplace_back(static_cast<u32>(i), static_cast<u32>(e - i));
        i = e;
    }
    return out;
}

#if !defined(__APPLE__) && !defined(_WIN32)

// Hunspell, opened by name the first time it is wanted (blit does not
// link it: without it, or without an English dictionary, nothing is
// checked).
namespace {

struct Hun {
    std::mutex mu;
    bool tried = false;
    void* lib = nullptr;
    void* h = nullptr;
    void* (*create)(const char*, const char*) = nullptr;
    int (*spell)(void*, const char*) = nullptr;
    int (*suggest)(void*, char***, const char*) = nullptr;
    void (*free_list)(void*, char***, int) = nullptr;
    int (*add)(void*, const char*) = nullptr;
    std::string words;  // blit's own list
};

Hun& hun() {
    static Hun x;
    return x;
}

std::string words_file() {
    const char* d = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    return (d && *d ? std::string(d) : std::string(home ? home : "") + "/.local/share") + "/blit/words";
}

// (Under its lock.)
bool open_it(Hun& x) {
    if (x.tried) return x.h != nullptr;
    x.tried = true;
    for (const char* name : {"libhunspell-1.7.so.0", "libhunspell-1.7.so", "libhunspell.so", "libhunspell-1.6.so.0"})
        if ((x.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
    if (!x.lib) return false;
    x.create = reinterpret_cast<void* (*)(const char*, const char*)>(dlsym(x.lib, "Hunspell_create"));
    x.spell = reinterpret_cast<int (*)(void*, const char*)>(dlsym(x.lib, "Hunspell_spell"));
    x.suggest = reinterpret_cast<int (*)(void*, char***, const char*)>(dlsym(x.lib, "Hunspell_suggest"));
    x.free_list = reinterpret_cast<void (*)(void*, char***, int)>(dlsym(x.lib, "Hunspell_free_list"));
    x.add = reinterpret_cast<int (*)(void*, const char*)>(dlsym(x.lib, "Hunspell_add"));
    if (!x.create || !x.spell || !x.suggest || !x.free_list || !x.add) return false;
    for (const std::string dir : {"/usr/share/hunspell", "/usr/share/myspell", "/usr/share/myspell/dicts", "/usr/local/share/hunspell"})
        for (const char* name : {"en_US", "en_GB", "en"}) {
            const std::string aff = dir + "/" + name + ".aff", dic = dir + "/" + name + ".dic";
            if (FILE* f = std::fopen(dic.c_str(), "rb")) {
                std::fclose(f);
                x.h = x.create(aff.c_str(), dic.c_str());
                if (x.h) break;
            }
        }
    if (!x.h) return false;
    if (FILE* f = std::fopen(words_file().c_str(), "rb")) {  // (the words added before)
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            std::string w(line);
            while (!w.empty() && (w.back() == '\n' || w.back() == '\r')) w.pop_back();
            if (!w.empty()) x.add(x.h, w.c_str());
        }
        std::fclose(f);
    }
    return true;
}

}  // namespace

bool spell_ready() {
    Hun& x = hun();
    std::lock_guard lock(x.mu);
    return open_it(x);
}

bool spell_ok(std::string_view word) {
    Hun& x = hun();
    std::lock_guard lock(x.mu);
    return !open_it(x) || x.spell(x.h, std::string(word).c_str()) != 0;
}

std::vector<std::string> spell_suggest(std::string_view word) {
    Hun& x = hun();
    std::lock_guard lock(x.mu);
    std::vector<std::string> out;
    if (!open_it(x)) return out;
    char** list = nullptr;
    const int n = x.suggest(x.h, &list, std::string(word).c_str());
    for (int i = 0; i < n && i < 12; ++i) out.emplace_back(list[i]);
    if (list) x.free_list(x.h, &list, n);
    return out;
}

void spell_learn(std::string_view word) {
    Hun& x = hun();
    std::lock_guard lock(x.mu);
    if (!open_it(x)) return;
    const std::string w(word);
    x.add(x.h, w.c_str());
    if (FILE* f = std::fopen(words_file().c_str(), "ab")) {
        std::fprintf(f, "%s\n", w.c_str());
        std::fclose(f);
    }
}

#elif defined(__APPLE__)

extern "C" {
int blit_spell_ready(void);
int blit_spell_ok(const char* s, size_t n);
int blit_spell_suggest(const char* s, size_t n, char* out, size_t room);
void blit_spell_learn(const char* s, size_t n);
}

bool spell_ready() {
    static const bool ready = blit_spell_ready() != 0;
    return ready;
}
bool spell_ok(std::string_view word) { return blit_spell_ok(word.data(), word.size()) != 0; }
std::vector<std::string> spell_suggest(std::string_view word) {
    char buf[2048];
    const int n = blit_spell_suggest(word.data(), word.size(), buf, sizeof buf);
    std::vector<std::string> out;
    for (int i = 0, at = 0; i < n; ++i) {
        out.emplace_back(buf + at);
        at += static_cast<int>(out.back().size()) + 1;
    }
    return out;
}
void spell_learn(std::string_view word) { blit_spell_learn(word.data(), word.size()); }

#else

bool spell_ready() { return false; }
bool spell_ok(std::string_view) { return true; }
std::vector<std::string> spell_suggest(std::string_view) { return {}; }
void spell_learn(std::string_view) {}

#endif

}  // namespace blit
