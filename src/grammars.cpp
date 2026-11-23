#include "grammars.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <tree_sitter/api.h>

#if !defined(_WIN32)
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "missing.hpp"
#include "qos.hpp"
extern char** environ;
#endif

namespace blit {

const std::vector<GrammarSource>& builtin_grammars() {
    // Each grammar's own repository, at the commit of its latest release
    // when this list was written (the tag is in the comment). A commit
    // names exact contents, so what gets compiled is what was looked at.
    static const std::vector<GrammarSource> list = {
        // The languages blit has built in: C, C++ (HIP files too) and Python.
        {.name = "c", .repo = "tree-sitter/tree-sitter-c", .rev = "b780e47fc780ddc8da13afa35a3f4ed5c157823d"},                         // v0.24.2
        {.name = "cpp", .repo = "tree-sitter/tree-sitter-cpp", .rev = "f41e1a044c8a84ea9fa8577fdd2eab92ec96de02", .inherits = {"c"}},  // v0.23.4
        {.name = "python", .repo = "tree-sitter/tree-sitter-python", .rev = "293fdc02038ee2bf0e2e206711b69c90ac0d413f"},               // v0.25.0
        // What those projects' other files are written in (init.lua too).
        {.name = "cmake", .repo = "uyha/tree-sitter-cmake", .rev = "a3ec2b2144daf7132e7ee824d8cab9610523d10f"},                        // v0.7.6
        {.name = "make", .repo = "tree-sitter-grammars/tree-sitter-make", .rev = "5e9e8f8ff3387b0edcaa90f46ddf3629f4cfeb1d"},          // v1.1.1
        {.name = "json", .repo = "tree-sitter/tree-sitter-json", .rev = "ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24"},                   // v0.24.8
        {.name = "toml", .repo = "tree-sitter-grammars/tree-sitter-toml", .rev = "64b56832c2cffe41758f28e05c756a3a98d16f41"},          // v0.7.0
        {.name = "yaml", .repo = "tree-sitter-grammars/tree-sitter-yaml", .rev = "7708026449bed86239b1cd5bce6e3c34dbca6415"},          // v0.7.2
        {.name = "markdown", .repo = "tree-sitter-grammars/tree-sitter-markdown", .rev = "41ff24a6c775492c543b09228aeb98d1cd2b3a0f",
         .path = "tree-sitter-markdown"},                                                                                              // v0.5.4
        {.name = "bash", .repo = "tree-sitter/tree-sitter-bash", .rev = "a06c2e4415e9bc0346c6b86d401879ffb44058f7"},                   // v0.25.1
        {.name = "lua", .repo = "tree-sitter-grammars/tree-sitter-lua", .rev = "10fe0054734eec83049514ea2e718b2a56acd0c9"},            // v0.5.0
        // Hardware: SystemVerilog, with Verilog (a subset) parsed by its
        // grammar under its own name, and Tcl for the tools' scripts and
        // constraints (.xdc, .sdc).
        {.name = "systemverilog", .repo = "gmlarumbe/tree-sitter-systemverilog", .rev = "306458a919c0409e0ee53d5bf8731eb69915d7d1",
         .display = "SystemVerilog"},                                                                                                  // v0.5.0
        {.name = "verilog", .parser = "systemverilog", .display = "Verilog"},
        // (No release yet: the commit is main's latest when this was written, 2026-11-09.)
        {.name = "tcl", .repo = "tree-sitter-grammars/tree-sitter-tcl", .rev = "90e87ed495a04fd0bf18db4be12b7033cd7402f6", .display = "Tcl"},
        // Shaders. Metal's language is C++ with more to it: C++'s grammar
        // under its own name. HLSL and Slang keep no highlight query of
        // their own; Neovim's are used (nvim-treesitter has made no release
        // since 2024: its main at 2026-11-21, which pins the same HLSL).
        {.name = "glsl", .repo = "tree-sitter-grammars/tree-sitter-glsl", .rev = "e47b8b62b59d0e3529f1c31b03e025d6bd475044", .inherits = {"c"},
         .display = "GLSL"},                                                                                                           // v0.2.0
        {.name = "hlsl", .repo = "tree-sitter-grammars/tree-sitter-hlsl", .rev = "bab9111922d53d43668fabb61869bec51bbcb915", .inherits = {"cpp"},
         .display = "HLSL", .queries = "nvim-treesitter/nvim-treesitter@52773f0d13a0c11d0c3ef33c14ab6ecc1c17148b:runtime/queries/hlsl"},  // v0.2.0
        // (No release: the commit is master's latest, 2023-01-09, which Neovim uses too.)
        {.name = "wgsl", .repo = "szebniok/tree-sitter-wgsl", .rev = "40259f3c77ea856841a4e0c4c807705f3e4a2b65", .display = "WGSL"},
        {.name = "slang", .repo = "tree-sitter-grammars/tree-sitter-slang", .rev = "c7ab1ee2bea2ac431fa78d72b80d8957d5a5bcd3", .inherits = {"c"},
         .display = "Slang", .queries = "nvim-treesitter/nvim-treesitter@52773f0d13a0c11d0c3ef33c14ab6ecc1c17148b:runtime/queries/slang"},  // v0.3.1
        {.name = "metal", .parser = "cpp", .display = "Metal"},
        // GPU kernels.
        {.name = "cuda", .repo = "tree-sitter-grammars/tree-sitter-cuda", .rev = "d58080a327756e4d1d16ec329ba7cb2048f6c6cd", .inherits = {"cpp"},
         .display = "CUDA"},                                                                                                           // v0.21.2
        // Firmware: assembly (GNU as and the like), linker scripts, devicetree and Kconfig.
        {.name = "asm", .repo = "RubixDev/tree-sitter-asm", .rev = "5bb5b03e3c1ce5853b5282b9fba060f7c7bbf11e", .display = "Assembly"},  // v0.24.0
        {.name = "linkerscript", .repo = "tree-sitter-grammars/tree-sitter-linkerscript", .rev = "f99011a3554213b654985a4b0a65b3b032ec4621",
         .display = "Linker script"},                                                                                                  // v1.0.0
        {.name = "devicetree", .repo = "joelspadin/tree-sitter-devicetree", .rev = "e685f1f6ac1702b046415efb476444167d63e41a",
         .display = "Devicetree"},                                                                                                     // v0.15.0
        {.name = "kconfig", .repo = "tree-sitter-grammars/tree-sitter-kconfig", .rev = "9934ff456d867d0d0f98ef365a0de410ff4e6705",
         .display = "Kconfig"},                                                                                                        // v1.3.0
    };
    return list;
}

std::vector<GrammarSource> grammar_list(const std::vector<GrammarSource>& from_settings) {
    std::vector<GrammarSource> list;
    const std::vector<GrammarSource>& builtin = builtin_grammars();
    for (GrammarSource g : from_settings) {
        for (const GrammarSource& b : builtin) {
            if (b.name != g.name) continue;
            // (A repository of its own makes an alias a grammar; another
            // repository leaves the built-in one's revision and queries.)
            if (g.parser.empty() && g.repo.empty()) g.parser = b.parser;
            if (g.repo.empty() && g.parser.empty()) g.repo = b.repo;
            if (g.rev.empty() && g.repo == b.repo) g.rev = b.rev;
            if (g.path.empty() && g.repo == b.repo) g.path = b.path;
            if (g.queries.empty() && g.repo == b.repo) g.queries = b.queries;
            if (g.inherits.empty()) g.inherits = b.inherits;
            if (g.comment.empty()) g.comment = b.comment;
            if (g.display.empty()) g.display = b.display;
        }
        list.push_back(std::move(g));
    }
    for (const GrammarSource& b : builtin)
        if (!find_grammar(from_settings, b.name)) list.push_back(b);
    return list;
}

const GrammarSource* find_grammar(const std::vector<GrammarSource>& list, std::string_view name) {
    for (const GrammarSource& g : list)
        if (g.name == name) return &g;
    return nullptr;
}

std::string grammar_parser(const std::vector<GrammarSource>& list, std::string_view name) {
    std::string at(name);
    for (int depth = 0; depth < 4; ++depth) {  // (an alias of an alias, but not round in a circle)
        const GrammarSource* g = find_grammar(list, at);
        if (!g || g->parser.empty() || g->parser == at) break;
        at = g->parser;
    }
    return at;
}

std::vector<std::pair<std::string, std::string>> grammar_parsers(const std::vector<GrammarSource>& list) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const GrammarSource& g : list)
        if (!g.parser.empty()) out.emplace_back(g.name, grammar_parser(list, g.name));
    return out;
}

std::string grammar_dir() {
    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return std::string(v ? v : "");
    };
#if defined(_WIN32)
    return env("LOCALAPPDATA") + "\\blit";
#else
    return (!env("XDG_DATA_HOME").empty() ? env("XDG_DATA_HOME") : env("HOME") + "/.local/share") + "/blit";
#endif
}

namespace {

// What the stamp beside an installed grammar says: where it came from.
std::string stamp_of(const GrammarSource& g) { return g.repo + "@" + g.rev + ":" + g.path + (g.queries.empty() ? "" : " " + g.queries); }

bool read_file(const std::string& path, std::string* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out->append(buf, n);
    std::fclose(f);
    return true;
}

bool write_file(const std::string& path, std::string_view text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// A name that is safe to put in a path and a command line.
bool plain_name(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+')) return false;
    return true;
}

#if !defined(_WIN32)
bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool make_dirs(const std::string& path) {
    for (size_t at = 1; at <= path.size(); ++at) {
        if (at != path.size() && path[at] != '/') continue;
        const std::string part = path.substr(0, at);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

// Runs a program and waits for it. What it printed (both streams) goes to
// *output. Returns its exit status, -2 if it could not be started (why in
// *output), or -1 if it went wrong otherwise.
int run(const std::vector<std::string>& argv, std::string* output) {
    int out[2];
    if (pipe(out) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawn_file_actions_addclose(&fa, out[1]);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    // (Nothing else this process has open goes into the program: see git.cpp.)
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        if (output) *output = rc == ENOENT && argv[0].find('/') == std::string::npos ? not_installed(argv[0], {}) : std::string(argv[0]) + ": " + std::strerror(rc);
        return -2;
    }
    char buf[4096];
    for (ssize_t n; (n = read(out[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR);)
        if (n > 0 && output && output->size() < 64 * 1024) output->append(buf, static_cast<size_t>(n));
    close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The last few lines of a tool's output, for an error message.
std::string tail(const std::string& text) {
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
    size_t at = t.size();
    for (int lines = 0; lines < 4 && at != std::string::npos && at > 0; ++lines) at = t.rfind('\n', at - 1);
    return at == std::string::npos || at == 0 ? t : t.substr(at + 1);
}

bool install_one(const GrammarSource& g, std::string* err, const std::function<void(const std::string&)>& say) {
    auto tell = [&](const std::string& what) {
        if (say) say(what);
    };
    if (!plain_name(g.name) || g.repo.empty() || g.rev.empty() || g.repo.find_first_of(" \t'\"\\") != std::string::npos ||
        g.rev.find_first_of(" \t'\"\\/") != std::string::npos || g.path.find("..") != std::string::npos) {
        *err = "the grammar's name, repo or rev is not usable";
        return false;
    }
    const std::string dir = grammar_dir();
    const std::string work = dir + "/build/" + g.name + "." + std::to_string(getpid());
    struct Cleanup {
        std::string path;
        ~Cleanup() {
            std::string ignored;
            run({"rm", "-rf", path}, &ignored);
        }
    } cleanup{work};
    std::string out;
    run({"rm", "-rf", work}, &out);
    if (!make_dirs(work + "/src") || !make_dirs(dir + "/parsers") || !make_dirs(dir + "/queries/" + g.name)) {
        *err = "cannot create folders under " + dir;
        return false;
    }

    const std::string url = "https://github.com/" + g.repo + "/archive/" + g.rev + ".tar.gz";
    tell("fetching " + url);
    out.clear();
    if (const int rc = run({"curl", "-fsSL", "--retry", "2", "--connect-timeout", "20", "--max-time", "300", "-o", work + "/src.tar.gz", url}, &out); rc != 0) {
        *err = rc == -2 ? out : "could not fetch " + url + (out.empty() ? "" : ": " + tail(out));
        return false;
    }
    out.clear();
    if (const int rc = run({"tar", "-xzf", work + "/src.tar.gz", "-C", work + "/src", "--strip-components=1"}, &out); rc != 0) {
        *err = rc == -2 ? out : "could not unpack the source: " + tail(out);
        return false;
    }

    const std::string top = work + "/src", base = g.path.empty() ? top : top + "/" + g.path, src = base + "/src";
    if (!exists(src + "/parser.c")) {
        *err = "no src/parser.c in " + g.repo + (g.path.empty() ? "" : "/" + g.path) + " (this repository may not include its generated parser)";
        return false;
    }
    // The highlight query: beside the grammar, or at the top of a
    // repository that holds several grammars.
    std::string query;
    bool have_query = false;
    if (!g.queries.empty()) {
        // Or from elsewhere, for a grammar that keeps none of its own: the
        // one file, from that repository at that commit.
        const size_t at = g.queries.find('@'), colon = g.queries.find(':', at == std::string::npos ? 0 : at);
        if (at == std::string::npos || colon == std::string::npos || g.queries.find_first_of(" \t'\"\\") != std::string::npos ||
            g.queries.find("..") != std::string::npos) {
            *err = "the grammar's queries are not usable: " + g.queries;
            return false;
        }
        const std::string from = "https://raw.githubusercontent.com/" + g.queries.substr(0, at) + "/" + g.queries.substr(at + 1, colon - at - 1) + "/" +
                                 g.queries.substr(colon + 1) + "/highlights.scm";
        tell("fetching " + from);
        out.clear();
        if (const int rc = run({"curl", "-fsSL", "--retry", "2", "--connect-timeout", "20", "--max-time", "120", "-o", work + "/highlights.scm", from}, &out); rc != 0) {
            *err = rc == -2 ? out : "could not fetch " + from + (out.empty() ? "" : ": " + tail(out));
            return false;
        }
        have_query = read_file(work + "/highlights.scm", &query);
    }
    for (const std::string& q : {base + "/queries/highlights.scm", top + "/queries/highlights.scm",
                                 base + "/queries/" + g.name + "/highlights.scm", top + "/queries/" + g.name + "/highlights.scm"}) {
        if (have_query || (have_query = read_file(q, &query))) break;
    }
    if (!have_query) {
        *err = "no queries/highlights.scm in " + g.repo;
        return false;
    }

    // Compile. The scanner, when a grammar has one, is C, or C++ in a few
    // older grammars.
    tell("building " + g.name);
    std::vector<std::string> objects;
    auto compile = [&](const char* driver, const std::string& file, const std::vector<std::string>& flags) {
        std::vector<std::string> cmd = {driver, "-O2", "-fPIC", "-w", "-I", src, "-c", file, "-o", file + ".o"};
        cmd.insert(cmd.begin() + 1, flags.begin(), flags.end());
        out.clear();
        if (const int rc = run(cmd, &out); rc != 0) {
            *err = rc == -2 ? out : std::string("the compiler failed on ") + file.substr(top.size() + 1) + ": " + tail(out);
            return false;
        }
        objects.push_back(file + ".o");
        return true;
    };
    bool cxx = false;
    if (!compile("cc", src + "/parser.c", {"-std=c11"})) return false;
    if (exists(src + "/scanner.c")) {
        if (!compile("cc", src + "/scanner.c", {"-std=c11"})) return false;
    } else if (exists(src + "/scanner.cc")) {
        cxx = true;
        if (!compile("c++", src + "/scanner.cc", {"-std=c++17"})) return false;
    }
    const std::string built = work + "/" + g.name + ".so";
    std::vector<std::string> link = {cxx ? "c++" : "cc", "-shared", "-o", built};
    link.insert(link.end(), objects.begin(), objects.end());
    out.clear();
    if (run(link, &out) != 0) {
        *err = "linking failed: " + tail(out);
        return false;
    }

    // Does it load, and is it a grammar this tree-sitter can use?
    void* lib = dlopen(built.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        *err = std::string("the built grammar does not load: ") + dlerror();
        return false;
    }
    using Entry = const TSLanguage* (*)();
    const Entry entry = reinterpret_cast<Entry>(dlsym(lib, ("tree_sitter_" + g.name).c_str()));
    const u32 abi = entry ? ts_language_abi_version(entry()) : 0;
    dlclose(lib);
    if (!entry) {
        *err = "the built grammar has no tree_sitter_" + g.name + "()";
        return false;
    }
    if (abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION || abi > TREE_SITTER_LANGUAGE_VERSION) {
        *err = "the grammar is version " + std::to_string(abi) + " of the grammar format; this tree-sitter reads " +
               std::to_string(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION) + " to " + std::to_string(TREE_SITTER_LANGUAGE_VERSION);
        return false;
    }

    // Into place: the query (after the queries it builds on), the grammar,
    // and a note of where they came from. Each is written beside its final
    // name and renamed, so a half-written file is never there to be read.
    // (A query that names what it inherits already, as Neovim's do, keeps its own line.)
    if (!g.inherits.empty() && !(query.starts_with(';') && query.substr(0, query.find('\n')).find("inherits:") != std::string::npos)) {
        std::string head = "; inherits: ";
        for (size_t i = 0; i < g.inherits.size(); ++i) head += (i ? "," : "") + g.inherits[i];
        query = head + "\n" + query;
    }
    const std::string query_path = dir + "/queries/" + g.name + "/highlights.scm", parser_path = dir + "/parsers/" + g.name + ".so";
    if (!write_file(query_path + ".new", query) || rename((query_path + ".new").c_str(), query_path.c_str()) != 0 ||
        rename(built.c_str(), parser_path.c_str()) != 0 || !write_file(dir + "/parsers/" + g.name + ".rev", stamp_of(g))) {
        *err = "cannot write into " + dir;
        return false;
    }
    tell("installed " + g.name);
    return true;
}
#endif

bool install_with_parents(const std::vector<GrammarSource>& list, const std::string& name, std::string* err,
                          const std::function<void(const std::string&)>& say, int depth) {
#if defined(_WIN32)
    (void)list;
    (void)name;
    (void)say;
    (void)depth;
    *err = "installing grammars is not available on Windows yet";
    return false;
#else
    const GrammarSource* g = find_grammar(list, name);
    if (!g) {
        *err = "blit does not know where to get a grammar for " + name + "; blit.opt.syntax.grammars." + name + " in init.lua can say";
        return false;
    }
    if (depth > 4) {
        *err = "grammars inherit from each other in a circle";
        return false;
    }
    for (const std::string& parent : g->inherits) {
        const GrammarSource* p = find_grammar(list, parent);
        if (p && grammar_state(*p) == GrammarState::kInstalled) continue;
        if (!install_with_parents(list, parent, err, say, depth + 1)) {
            *err = "needs " + parent + " first: " + *err;
            return false;
        }
    }
    return install_one(*g, err, say);
#endif
}

}  // namespace

GrammarState grammar_state(const GrammarSource& g) {
    const std::string dir = grammar_dir();
    std::string stamp, ignored;
    if (!read_file(dir + "/parsers/" + g.name + ".rev", &stamp)) return GrammarState::kMissing;
    FILE* parser = std::fopen((dir + "/parsers/" + g.name + ".so").c_str(), "rb");
    if (!parser) return GrammarState::kMissing;
    std::fclose(parser);
    FILE* query = std::fopen((dir + "/queries/" + g.name + "/highlights.scm").c_str(), "rb");
    if (!query) return GrammarState::kMissing;
    std::fclose(query);
    return stamp == stamp_of(g) ? GrammarState::kInstalled : GrammarState::kOutdated;
}

bool install_grammar(const std::vector<GrammarSource>& list, const std::string& name, std::string* err,
                     const std::function<void(const std::string&)>& say) {
    std::string local;
    return install_with_parents(list, grammar_parser(list, name), err ? err : &local, say, 0);
}

int install_grammars_command(const std::vector<GrammarSource>& list, std::vector<std::string> names) {
    if (std::find(names.begin(), names.end(), "all") != names.end()) {
        names.clear();
        for (const GrammarSource& g : list) names.push_back(g.name);
    }
    int failed = 0;
    for (const std::string& name : names) {
        const GrammarSource* g = find_grammar(list, grammar_parser(list, name));  // (an alias: the grammar it uses)
        if (g && grammar_state(*g) == GrammarState::kInstalled) {
            std::printf("%-13s already installed\n", name.c_str());
            continue;
        }
        std::string err;
        const bool ok = install_grammar(list, name, &err, [&](const std::string& step) {
            std::printf("%-13s %s\n", name.c_str(), step.c_str());
            std::fflush(stdout);
        });
        if (!ok) {
            std::printf("%-13s FAILED: %s\n", name.c_str(), err.c_str());
            ++failed;
        }
    }
    return failed ? 1 : 0;
}

void print_grammars(const std::vector<GrammarSource>& list) {
    std::printf("grammars are installed into %s\n", grammar_dir().c_str());
    for (const GrammarSource& g : list) {
        if (!g.parser.empty()) {
            std::printf("  %-13s %-10s the %s grammar\n", g.name.c_str(), "", grammar_parser(list, g.name).c_str());
            continue;
        }
        const GrammarState st = grammar_state(g);
        std::printf("  %-13s %-10s %s%s%s @ %.12s\n", g.name.c_str(),
                    st == GrammarState::kInstalled ? "installed" : st == GrammarState::kOutdated ? "outdated" : "-", g.repo.c_str(),
                    g.path.empty() ? "" : "/", g.path.c_str(), g.rev.c_str());
    }
}

// ---------------------------------------------------------------- in the background

struct GrammarInstaller::State {
    std::mutex mu;
    std::condition_variable cv;
    struct Job {
        std::vector<GrammarSource> list;
        std::string name;
    };
    std::vector<Job> queue;
    std::vector<Done> done;
    std::vector<std::string> asked;  // queued, under way or failed: not to be queued again
    std::string current;
    std::function<void()> notify;
    bool stop = false, running = false;

    static void work(std::shared_ptr<State> s) {
        thread_soon(Soon::kBackground);
        for (;;) {
            Job job;
            {
                std::unique_lock lock(s->mu);
                if (s->stop || s->queue.empty()) {
                    s->running = false;
                    s->current.clear();
                    return;
                }
                job = std::move(s->queue.front());
                s->queue.erase(s->queue.begin());
                s->current = job.name;
            }
            Done d;
            d.name = job.name;
            d.ok = install_grammar(job.list, job.name, &d.message);
            std::function<void()> notify;
            {
                std::lock_guard lock(s->mu);
                // A success may be asked for again later (after an update);
                // a failure stays on the list until forgive().
                if (d.ok) std::erase(s->asked, d.name);
                s->done.push_back(std::move(d));
                notify = s->notify;
            }
            if (notify) notify();
        }
    }
};

GrammarInstaller::GrammarInstaller() : s_(std::make_shared<State>()) {}

GrammarInstaller::~GrammarInstaller() {
    // A fetch or compile under way is left to finish on its own; the thread
    // holds the state it needs and the program does not wait for it.
    std::lock_guard lock(s_->mu);
    s_->stop = true;
    s_->queue.clear();
    s_->notify = nullptr;
}

void GrammarInstaller::set_notify(std::function<void()> fn) {
    std::lock_guard lock(s_->mu);
    s_->notify = std::move(fn);
}

void GrammarInstaller::request(const std::vector<GrammarSource>& list, const std::string& name) {
    std::lock_guard lock(s_->mu);
    if (std::find(s_->asked.begin(), s_->asked.end(), name) != s_->asked.end()) return;
    s_->asked.push_back(name);
    s_->queue.push_back({list, name});
    if (!s_->running) {
        s_->running = true;
        std::thread(State::work, s_).detach();
    }
}

void GrammarInstaller::forgive() {
    std::lock_guard lock(s_->mu);
    std::erase_if(s_->asked, [&](const std::string& n) {
        if (n == s_->current) return false;
        for (const State::Job& j : s_->queue)
            if (j.name == n) return false;
        return true;
    });
}

std::vector<GrammarInstaller::Done> GrammarInstaller::poll() {
    std::lock_guard lock(s_->mu);
    std::vector<Done> out;
    out.swap(s_->done);
    return out;
}

std::string GrammarInstaller::current() const {
    std::lock_guard lock(s_->mu);
    return s_->current;
}

}  // namespace blit
