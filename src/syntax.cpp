#include "syntax.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>

#include "config.hpp"
#include "grammars.hpp"
#include "match.hpp"
#include "qos.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif
#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace blit {

namespace {

// ---------------------------------------------------------------- tree-sitter's memory

#if defined(__APPLE__)
// Compiling a highlight query has tree-sitter ask for a table of 4.4 MB for
// a moment, five at once while the query is compiled in parts. macOS's
// malloc keeps memory that size, once given back, as it was: 21 MB that the
// program held for the rest of its life (a quarter of what it holds when
// idle). Blocks that large are taken from the system directly, and go back
// to it when freed. Each has its size in front of it; one of them is told by
// where it starts in a page and by malloc not knowing it.
constexpr size_t kBig = 1u << 20;
constexpr uintptr_t kHead = 16;
bool ts_big(void* p) { return p && (reinterpret_cast<uintptr_t>(p) & 0x3FFF) == kHead && malloc_size(p) == 0; }
size_t ts_big_size(void* p) { return *reinterpret_cast<size_t*>(static_cast<char*>(p) - kHead); }
void* ts_big_alloc(size_t n) {
    void* m = mmap(nullptr, n + kHead, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);  // (zeroed, page aligned)
    if (m == MAP_FAILED) return nullptr;
    *static_cast<size_t*>(m) = n;
    return static_cast<char*>(m) + kHead;
}
void ts_big_free(void* p) { munmap(static_cast<char*>(p) - kHead, ts_big_size(p) + kHead); }
void* ts_alloc(size_t n) { return n >= kBig ? ts_big_alloc(n) : std::malloc(n); }
void* ts_zalloc(size_t count, size_t size) { return count * size >= kBig ? ts_big_alloc(count * size) : std::calloc(count, size); }
void ts_release(void* p) {
    if (ts_big(p)) ts_big_free(p);
    else std::free(p);
}
void* ts_resize(void* p, size_t n) {
    const bool big = ts_big(p);
    if (!big && n < kBig) return std::realloc(p, n);
    if (big && n >= kBig && n <= ts_big_size(p)) return p;
    void* q = ts_alloc(n);
    if (q && p) std::memcpy(q, p, std::min(n, big ? ts_big_size(p) : malloc_size(p)));
    if (q) ts_release(p);
    return q;
}
// (Before tree-sitter is first used: at start.)
const bool ts_allocator_set = (ts_set_allocator(ts_alloc, ts_zalloc, ts_resize, ts_release), true);
#endif

constexpr u8 kSkip = 0xFF;  // a capture that is not a colour (@spell, @_internal, ...)
// The styles a bracket gets by how deep it is, round and round.
constexpr const char* kBracketStyles[] = {"bracket.0", "bracket.1", "bracket.2"};
constexpr u32 kBracketDepths = 3;

// ---------------------------------------------------------------- files and folders

bool file_exists(const std::string& path) {
#if defined(_WIN32)
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool read_file(const std::string& path, std::string* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out->append(buf, n);
    std::fclose(f);
    return true;
}


// Where grammars and queries are looked for: folders from the settings, then
// the one beside init.lua (for grammars put there by hand), then the one
// blit installs into (see grammars.hpp).
std::vector<std::string> search_dirs(const std::vector<std::string>& extra, const char* sub) {
    std::vector<std::string> d = extra;
    const std::string cfg = default_init_path();
    const size_t slash = cfg.find_last_of("/\\");
    if (slash != std::string::npos) d.push_back(cfg.substr(0, slash + 1) + sub);
    d.push_back(grammar_dir() + "/" + sub);
    return d;
}

std::vector<std::string> parser_dirs(const SyntaxOptions& o) { return search_dirs(o.parser_dirs, "parsers"); }

std::vector<std::string> query_dirs(const SyntaxOptions& o) { return search_dirs(o.query_dirs, "queries"); }

std::string find_parser(const std::string& lang, const SyntaxOptions& o) {
    for (const std::string& dir : parser_dirs(o))
        for (const char* ext : {".so", ".dylib", ".dll"}) {
            std::string p = dir + "/" + lang + ext;
            if (file_exists(p)) return p;
        }
    return {};
}

// ---------------------------------------------------------------- grammars

// Grammars are shared libraries exporting tree_sitter_<name>(). Once loaded
// they stay loaded.
const TSLanguage* load_language(const std::string& lang, const SyntaxOptions& o, std::string* err) {
    static std::mutex mu;
    static std::map<std::string, const TSLanguage*> loaded;
    std::lock_guard lk(mu);
    if (auto it = loaded.find(lang); it != loaded.end()) return it->second;

    const std::string path = find_parser(lang, o);
    if (path.empty()) {
        *err = "no tree-sitter grammar installed for " + lang;
        return nullptr;
    }
    std::string symbol = "tree_sitter_" + lang;
    std::replace(symbol.begin(), symbol.end(), '-', '_');
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(path.c_str());
    void* fn = lib ? reinterpret_cast<void*>(GetProcAddress(lib, symbol.c_str())) : nullptr;
#else
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    void* fn = lib ? dlsym(lib, symbol.c_str()) : nullptr;
#endif
    if (!fn) {
        *err = "cannot load grammar " + path;
        return nullptr;
    }
    const TSLanguage* l = reinterpret_cast<const TSLanguage* (*)()>(fn)();
    const u32 abi = l ? ts_language_abi_version(l) : 0;
    if (abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION || abi > TREE_SITTER_LANGUAGE_VERSION) {
        *err = "grammar " + path + " was built for another tree-sitter version";
        return nullptr;
    }
    loaded[lang] = l;
    return l;
}

// ---------------------------------------------------------------- query text

// Reads <dir>/<lang>/highlights.scm from the first folder at or after
// `from` that has it, with any "; inherits: a,b" parents in front.
bool load_query(const std::string& lang, const std::vector<std::string>& dirs, size_t from, std::string* out, int depth) {
    if (depth > 6) return false;
    std::string text;
    size_t d = from;
    for (; d < dirs.size(); ++d)
        if (read_file(dirs[d] + "/" + lang + "/highlights.scm", &text)) break;
    if (d == dirs.size()) return false;

    // Leading comment lines may name languages whose queries come first.
    size_t at = 0;
    while (at < text.size() && text[at] == ';') {
        const size_t nl = std::min(text.find('\n', at), text.size());
        std::string_view line(text.data() + at, nl - at);
        const size_t key = line.find("inherits:");
        if (key != std::string_view::npos) {
            std::string name;
            for (char c : std::string(line.substr(key + 9)) + ",") {
                if (c == ',') {
                    if (!name.empty()) load_query(name, dirs, 0, out, depth + 1);  // optional ones may be missing
                    name.clear();
                } else if (c != ' ' && c != '(' && c != ')' && c != '\r' && c != '\t') {
                    name += c;
                }
            }
        }
        at = nl + 1;
    }
    *out += text;
    *out += '\n';
    return true;
}

// Walks query text outside strings and comments. `on_pattern(offset)` is
// called where each top-level pattern starts, `on_capture(name)` for each
// @capture.
template <class OnPattern, class OnCapture>
void scan_query(std::string_view s, OnPattern&& on_pattern, OnCapture&& on_capture) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ';') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '"') {
            if (depth == 0) on_pattern(i);
            for (++i; i < s.size() && s[i] != '"'; ++i)
                if (s[i] == '\\') ++i;
        } else if (c == '(' || c == '[') {
            if (depth++ == 0) on_pattern(i);
        } else if (c == ')' || c == ']') {
            --depth;
        } else if (c == '@') {
            size_t j = i + 1;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.' || s[j] == '-')) ++j;
            on_capture(s.substr(i + 1, j - i - 1));
            i = j - 1;
        }
    }
}

// Style id for a capture name: 0 clears (@none), kSkip is not a colour.
u8 style_of(std::string_view name, const std::vector<std::string>& names) {
    if (name == "none") return 0;
    if (name.empty() || name[0] == '_' || name == "spell" || name == "nospell" || name.starts_with("conceal")) return kSkip;
    const size_t id = static_cast<size_t>(std::find(names.begin(), names.end(), name) - names.begin());
    return id < names.size() ? static_cast<u8>(id) : kSkip;
}

// ---------------------------------------------------------------- compiled queries

struct Pred {
    enum Kind : u8 { kEq, kAnyOf, kLua, kRegex, kContains, kAncestor, kParent } kind;
    bool negate = false;
    bool any = false;            // one matching node is enough (default: all must match)
    bool other_capture = false;  // kEq against another capture's text
    u32 capture = 0, capture2 = 0;
    std::vector<std::string> args;
    std::vector<Regex> regex;
};

struct Pattern {
    std::vector<Pred> preds;
    int priority = 100;
    bool disabled = false;  // has a predicate blit cannot evaluate
};

// One compiled query: the whole highlights file, or a slice of its patterns.
// Immutable once built, so any number of documents can share it.
struct QuerySet {
    TSQuery* query = nullptr;
    std::vector<u8> capture_style;  // per query capture: style id, 0 for @none, or kSkip
    std::vector<Pattern> patterns;
    u32 pattern_base = 0;  // index of its first pattern within the whole file

    QuerySet() = default;
    QuerySet(QuerySet&& o) noexcept
        : query(o.query), capture_style(std::move(o.capture_style)), patterns(std::move(o.patterns)), pattern_base(o.pattern_base) {
        o.query = nullptr;
    }
    QuerySet& operator=(QuerySet&&) = delete;
    ~QuerySet() {
        if (query) ts_query_delete(query);
    }
};

void compile_predicates(QuerySet& qs) {
    TSQuery* query = qs.query;
    const u32 npat = ts_query_pattern_count(query);
    qs.patterns.resize(npat);
    for (u32 pi = 0; pi < npat; ++pi) {
        u32 nsteps = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(query, pi, &nsteps);
        Pattern& pat = qs.patterns[pi];
        for (u32 at = 0; at < nsteps;) {
            u32 end = at;
            while (end < nsteps && steps[end].type != TSQueryPredicateStepTypeDone) ++end;
            auto str = [&](u32 k) {
                u32 len = 0;
                const char* s = ts_query_string_value_for_id(query, steps[k].value_id, &len);
                return std::string(s, len);
            };
            if (end > at && steps[at].type == TSQueryPredicateStepTypeString) {
                std::string name = str(at);
                if (name == "set!") {
                    if (end - at == 3 && steps[at + 1].type == TSQueryPredicateStepTypeString && str(at + 1) == "priority")
                        pat.priority = std::clamp(std::atoi(str(at + 2).c_str()), 0, 255);
                } else if (name.ends_with("!")) {
                    // other directives (offsets, conceal) do not change colours
                } else if (name == "is-not?" || name == "is?") {
                    // Properties of a node, of which grammars use one:
                    // "local", for names a locals query has seen defined.
                    // blit runs no locals query, so nothing is ever local:
                    // "is-not? local" always holds, "is? local" never does.
                    if (name == "is?") pat.disabled = true;
                } else if (end - at >= 3 && steps[at + 1].type == TSQueryPredicateStepTypeCapture) {
                    Pred p{};
                    if (name.starts_with("not-")) {
                        p.negate = true;
                        name.erase(0, 4);
                    }
                    if (name.starts_with("any-") && name != "any-of?") {
                        p.any = true;
                        name.erase(0, 4);
                    }
                    p.capture = steps[at + 1].value_id;
                    bool known = true;
                    if (name == "eq?") p.kind = Pred::kEq;
                    else if (name == "any-of?") p.kind = Pred::kAnyOf;
                    else if (name == "lua-match?") p.kind = Pred::kLua;
                    else if (name == "match?" || name == "vim-match?") p.kind = Pred::kRegex;
                    else if (name == "contains?") p.kind = Pred::kContains;
                    else if (name == "has-ancestor?") p.kind = Pred::kAncestor;
                    else if (name == "has-parent?") p.kind = Pred::kParent;
                    else known = false;
                    for (u32 k = at + 2; k < end && known; ++k) {
                        if (steps[k].type == TSQueryPredicateStepTypeCapture) {
                            if (p.kind == Pred::kEq && k == at + 2) {
                                p.other_capture = true;
                                p.capture2 = steps[k].value_id;
                            } else {
                                known = false;
                            }
                        } else {
                            p.args.push_back(str(k));
                        }
                    }
                    if (known && p.kind == Pred::kRegex) {
                        for (const std::string& a : p.args) {
                            p.regex.emplace_back(a);
                            known = known && p.regex.back().ok();
                        }
                    }
                    if (known && (p.other_capture || !p.args.empty())) pat.preds.push_back(std::move(p));
                    else pat.disabled = true;
                } else {
                    pat.disabled = true;  // a test in a shape blit does not know
                }
            }
            at = end + 1;
        }
    }
}

bool compile_set(const TSLanguage* language, std::string_view src, const std::vector<std::string>& names, u32 base,
                 QuerySet* out, u32* err_off) {
    TSQueryError err_type = TSQueryErrorNone;
    out->query = ts_query_new(language, src.data(), static_cast<u32>(src.size()), err_off, &err_type);
    if (!out->query) return false;
    const u32 ncap = ts_query_capture_count(out->query);
    out->capture_style.resize(ncap);
    for (u32 i = 0; i < ncap; ++i) {
        u32 len = 0;
        const char* s = ts_query_capture_name_for_id(out->query, i, &len);
        out->capture_style[i] = style_of(std::string_view(s, len), names);
    }
    out->pattern_base = base;
    compile_predicates(*out);
    return true;
}

// Everything compiled for one language, shared by all documents in it and
// kept for the life of the program. Compiling happens on background threads;
// users wait on `cv` for whichever of the two forms they can use.
//
// Why two forms: compiling a big highlight query in one piece is slow (the
// C++ one takes about 100 ms), and tree-sitter cannot do it in parallel. Cut
// into a few slices it compiles on several threads in under half the time,
// but running several queries costs a tree walk each, so the slices are only
// a stopgap until the whole query is ready.
struct Bundle {
    const TSLanguage* language = nullptr;
    std::vector<std::string> names{""};  // style names; index = style id
    std::mutex mu;
    std::condition_variable cv;
    bool whole_done = false, parts_done = false;
    bool parts_usable = true;     // false once the whole query turned out to come from another file
    std::vector<QuerySet> whole;  // one entry when it compiled
    std::vector<QuerySet> parts;  // the slices, when all of them compiled
    std::string error;
};

std::shared_ptr<Bundle> get_bundle(const std::string& lang, const SyntaxOptions& opt, std::string* err) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<Bundle>> bundles;
    std::string key = lang + '\n' + static_cast<char>('0' + opt.compile);
    for (const std::string& d : opt.parser_dirs) key += '\n' + d;
    for (const std::string& d : opt.query_dirs) key += '\n' + d;
    std::lock_guard lk(mu);
    if (auto it = bundles.find(key); it != bundles.end()) return it->second;

    auto b = std::make_shared<Bundle>();
    b->language = load_language(lang, opt, err);
    if (!b->language) return nullptr;

    // Every folder's version of the query, best first. Later ones are only
    // tried if an earlier one does not fit the grammar.
    const std::vector<std::string> dirs = query_dirs(opt);
    auto sources = std::make_shared<std::vector<std::string>>();
    std::string first_dir;
    for (size_t d = 0; d < dirs.size(); ++d) {
        if (!file_exists(dirs[d] + "/" + lang + "/highlights.scm")) continue;
        std::string src;
        if (load_query(lang, dirs, d, &src, 0)) {
            if (sources->empty()) first_dir = dirs[d];
            sources->push_back(std::move(src));
        }
    }
    if (sources->empty()) {
        *err = "no highlight query installed for " + lang;
        return nullptr;
    }
    auto collect_names = [](std::string_view src, std::vector<std::string>* names) {
        names->assign(1, "");
        scan_query(src, [](size_t) {}, [&](std::string_view name) {
            if (name == "none" || name.empty() || name[0] == '_' || name == "spell" || name == "nospell" || name.starts_with("conceal")) return;
            if (names->size() < 250 && std::find(names->begin(), names->end(), name) == names->end()) names->emplace_back(name);
        });
        for (const char* style : kBracketStyles)  // (brackets by depth: painted by the worker, not the query)
            if (std::find(names->begin(), names->end(), style) == names->end()) names->emplace_back(style);
    };
    collect_names(sources->front(), &b->names);

    // The slices: consecutive runs of top-level patterns of the first source.
    std::vector<size_t> starts;
    scan_query(sources->front(), [&](size_t at) { starts.push_back(at); }, [](std::string_view) {});
    const unsigned hw = std::thread::hardware_concurrency();
    u32 nparts = opt.compile == SyntaxOptions::kWholeOnly ? 0 : hw >= 8 ? 4 : hw >= 4 ? 2 : 0;
    if (opt.compile == SyntaxOptions::kPartsOnly) nparts = 4;
    if (starts.size() < nparts * 4) nparts = 0;
    if (nparts == 0) b->parts_done = true;
    if (opt.compile == SyntaxOptions::kPartsOnly && nparts > 0) b->whole_done = true;
    const auto names0 = std::make_shared<const std::vector<std::string>>(b->names);

    if (!b->whole_done) {
        std::thread([b, sources, lang, first_dir, names0, collect_names] {
            std::vector<QuerySet> done;
            std::vector<std::string> names = *names0;
            u32 first_err = 0;
            for (size_t i = 0; i < sources->size() && done.empty(); ++i) {
                if (i > 0) collect_names((*sources)[i], &names);
                QuerySet qs;
                u32 err_off = 0;
                if (compile_set(b->language, (*sources)[i], names, 0, &qs, &err_off)) done.push_back(std::move(qs));
                else if (i == 0) first_err = err_off;
            }
            std::lock_guard lk2(b->mu);
            if (done.empty())
                b->error = "highlight query for " + lang + " in " + first_dir + " does not fit the grammar (offset " +
                           std::to_string(first_err) + ")";
            else if (names != b->names) {  // an alternative query was used: the slices do not match it
                b->names = std::move(names);
                b->parts_usable = false;  // (left in place: a document may be reading them right now)
            }
            b->whole = std::move(done);
            b->whole_done = true;
            b->cv.notify_all();
        }).detach();
    }
    if (nparts > 0) {
        auto built = std::make_shared<std::vector<QuerySet>>(nparts);
        auto left = std::make_shared<std::atomic<u32>>(nparts);
        auto ok = std::make_shared<std::atomic<bool>>(true);
        for (u32 k = 0; k < nparts; ++k) {
            const size_t p0 = starts.size() * k / nparts, p1 = starts.size() * (k + 1) / nparts;
            const size_t from = starts[p0], to = p1 < starts.size() ? starts[p1] : sources->front().size();
            std::thread([b, sources, built, left, ok, names0, k, from, to, p0, p1] {
                u32 err_off = 0;
                QuerySet& qs = (*built)[k];
                const std::string_view src(sources->front().data() + from, to - from);
                if (!compile_set(b->language, src, *names0, static_cast<u32>(p0), &qs, &err_off) ||
                    ts_query_pattern_count(qs.query) != p1 - p0)
                    ok->store(false);
                if (left->fetch_sub(1) == 1) {  // the last slice to finish hands them all over
                    std::lock_guard lk2(b->mu);
                    if (ok->load() && b->parts_usable) b->parts = std::move(*built);
                    b->parts_done = true;
                    b->cv.notify_all();
                }
            }).detach();
        }
    }
    bundles[key] = b;
    return b;
}

struct Cap {
    u32 start, end;
    u32 pattern;  // index within the whole query file
    u8 style;
    u8 priority;
};

}  // namespace

// ---------------------------------------------------------------- snapshot

void Syntax::Snapshot::index() {
    starts.resize(pieces.size());
    u32 at = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        starts[i] = at;
        at += pieces[i].len;
    }
    size = at;
    hint = 0;
}

const char* Syntax::Snapshot::read(u32 byte, u32* len) const {
    if (byte >= size) {
        *len = 0;
        return "";
    }
    size_t i = hint;
    auto holds = [&](size_t k) { return k < pieces.size() && starts[k] <= byte && byte - starts[k] < pieces[k].len; };
    if (!holds(i)) {
        if (holds(i + 1)) ++i;  // the usual case: reading on from the previous piece
        else i = static_cast<size_t>(std::upper_bound(starts.begin(), starts.end(), byte) - starts.begin()) - 1;
        hint = i;
    }
    const u32 local = byte - starts[i];
    *len = pieces[i].len - local;
    return pieces[i].p + local;
}

void Syntax::Snapshot::copy(u32 start, u32 len, std::string* out) const {
    out->clear();
    while (len > 0) {
        u32 n;
        const char* p = read(start, &n);
        if (n == 0) break;
        n = std::min(n, len);
        out->append(p, n);
        start += n;
        len -= n;
    }
}

// ---------------------------------------------------------------- engine

// Everything the worker thread owns for one document.
struct Syntax::Engine {
    std::shared_ptr<Bundle> bundle;
    TSParser* parser = nullptr;
    TSTree* tree = nullptr;
    Snapshot snap;

    const std::vector<QuerySet>* sets = nullptr;  // the whole query, or its slices
    std::vector<TSQueryCursor*> cursors;
    bool names_sent = false;
    bool failed = false;

    // For has-parent? / has-ancestor?: a cursor that stays near the last
    // node asked about, so finding a parent does not start from the root.
    TSTreeCursor walk{};
    bool walking = false;

    // Style of every byte of the highlighted range, kept between passes so a
    // small pass can update part of it.
    std::vector<u8> paint;
    u32 paint_lo = 0, paint_hi = 0;
    bool paint_valid = false;

    std::vector<Cap> caps;
    std::string text, text2;

    // Brackets' styles by depth (0: no such style), and the shape last
    // made, to go out with the colours.
    u8 bracket_style[kBracketDepths] = {};
    SyntaxStructure shaped;
    // What shape() needs to know of each kind of node, worked out from its
    // type's name the first time one is met.
    struct Kind {
        bool known = false;
        u8 definition = 0;  // see definition_kind()
        u8 fold = 0;        // 1 a comment, 2 a block, 3 a #if
        char bracket = 0;   // an anonymous node that is one bracket
    };
    std::vector<Kind> kinds;
    const Kind& kind_of(TSNode n);

    ~Engine() {
        for (TSQueryCursor* c : cursors) ts_query_cursor_delete(c);
        if (walking) ts_tree_cursor_delete(&walk);
        if (tree) ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
    }

    // The node's text, without copying when it lies in one piece.
    std::string_view node_text(TSNode n, std::string* buf, u32 cap = 1u << 16) {
        const u32 s = ts_node_start_byte(n), len = std::min(ts_node_end_byte(n) - s, cap);
        u32 avail = 0;
        const char* p = snap.read(s, &avail);
        if (avail >= len) return {p, len};
        snap.copy(s, len, buf);
        return *buf;
    }

    void use(const std::vector<QuerySet>* next) {
        for (TSQueryCursor* c : cursors) ts_query_cursor_delete(c);
        cursors.clear();
        sets = next;
        for (size_t i = 0; i < sets->size(); ++i) {
            cursors.push_back(ts_query_cursor_new());
            ts_query_cursor_set_match_limit(cursors.back(), 256);
        }
    }

    bool seek(TSNode node);
    bool ancestor_listed(const Pred& p, TSNode node);
    bool test(const Pred& p, const TSQueryMatch& m, TSNode node);
    bool accept(const Pattern& pat, const TSQueryMatch& m);
    template <class Stale>
    bool collect(u32 lo, u32 hi, Stale&& stale);
    void paint_caps(u32 lo, u32 hi);
    void shift_paint(const TextEdit& e);
    void marks(Highlights* out) const;
    void shape(u32 lo, u32 hi, SyntaxStructure* out, bool paint_brackets);
};

// Puts `walk` on the node. From there its parents are a step away.
bool Syntax::Engine::seek(TSNode node) {
    const u32 s = ts_node_start_byte(node), e = ts_node_end_byte(node);
    if (s == e) return false;  // empty nodes cannot be found by position
    if (!walking) {
        walk = ts_tree_cursor_new(ts_tree_root_node(tree));
        walking = true;
    }
    for (;;) {  // up until the current node contains the target
        const TSNode cur = ts_tree_cursor_current_node(&walk);
        if (ts_node_start_byte(cur) <= s && ts_node_end_byte(cur) >= e) break;
        if (!ts_tree_cursor_goto_parent(&walk)) break;
    }
    for (int depth = 0; depth < 4096; ++depth) {  // then down to it
        const TSNode cur = ts_tree_cursor_current_node(&walk);
        if (ts_node_eq(cur, node)) return true;
        if (ts_tree_cursor_goto_first_child_for_byte(&walk, s) < 0) return false;
        const TSNode child = ts_tree_cursor_current_node(&walk);
        if (ts_node_start_byte(child) > s || ts_node_end_byte(child) < e) return false;
    }
    return false;
}

bool Syntax::Engine::ancestor_listed(const Pred& p, TSNode node) {
    auto listed = [&](TSNode n) {
        const char* t = ts_node_type(n);
        for (const std::string& a : p.args)
            if (a == t) return true;
        return false;
    };
    if (!seek(node)) {  // rare: fall back to tree-sitter's own, slower, lookup
        for (TSNode n = ts_node_parent(node); !ts_node_is_null(n); n = ts_node_parent(n)) {
            if (listed(n)) return true;
            if (p.kind == Pred::kParent) break;
        }
        return false;
    }
    if (p.kind == Pred::kParent) return ts_tree_cursor_goto_parent(&walk) && listed(ts_tree_cursor_current_node(&walk));
    TSTreeCursor up = ts_tree_cursor_copy(&walk);  // climb on a copy; `walk` stays put for the next lookup
    bool found = false;
    while (!found && ts_tree_cursor_goto_parent(&up)) found = listed(ts_tree_cursor_current_node(&up));
    ts_tree_cursor_delete(&up);
    return found;
}

bool Syntax::Engine::test(const Pred& p, const TSQueryMatch& m, TSNode node) {
    switch (p.kind) {
        case Pred::kAncestor:
        case Pred::kParent: return ancestor_listed(p, node);
        case Pred::kEq:
        case Pred::kAnyOf: {
            const u32 len = ts_node_end_byte(node) - ts_node_start_byte(node);
            if (p.other_capture) {
                for (u16 k = 0; k < m.capture_count; ++k)
                    if (m.captures[k].index == p.capture2) return node_text(node, &text) == node_text(m.captures[k].node, &text2);
                return false;
            }
            bool possible = false;  // only look at the text if some argument has its length
            for (const std::string& a : p.args) possible = possible || a.size() == len;
            if (!possible) return false;
            const std::string_view t = node_text(node, &text);
            for (const std::string& a : p.args)
                if (a == t) return true;
            return false;
        }
        case Pred::kLua: return lua_match(node_text(node, &text), p.args[0]);
        case Pred::kRegex: return p.regex[0].search(node_text(node, &text));
        case Pred::kContains: {
            const std::string_view t = node_text(node, &text);
            for (const std::string& a : p.args)
                if (t.find(a) != std::string_view::npos) return true;
            return false;
        }
    }
    return false;
}

bool Syntax::Engine::accept(const Pattern& pat, const TSQueryMatch& m) {
    for (const Pred& p : pat.preds) {
        bool seen = false, all = true, any = false;
        for (u16 k = 0; k < m.capture_count; ++k) {
            if (m.captures[k].index != p.capture) continue;
            seen = true;
            const bool r = test(p, m, m.captures[k].node) != p.negate;
            all = all && r;
            any = any || r;
        }
        if (seen && !(p.any ? any : all)) return false;
    }
    return true;
}

// Runs the highlight query over bytes [lo, hi) and gathers the captures in
// painting order. Returns false if `stale()` said to give up.
template <class Stale>
bool Syntax::Engine::collect(u32 lo, u32 hi, Stale&& stale) {
    caps.clear();
    bool mixed = false;
    u32 seen = 0;
    const TSNode root = ts_tree_root_node(tree);
    for (size_t k = 0; k < sets->size(); ++k) {
        const QuerySet& qs = (*sets)[k];
        TSQueryCursor* cursor = cursors[k];
        ts_query_cursor_set_byte_range(cursor, lo, hi);
        ts_query_cursor_exec(cursor, qs.query, root);
        TSQueryMatch m;
        u32 ci = 0;
        // The captures of one match arrive one at a time; remember the verdict.
        u32 memo_id = ~0u, memo_pattern = ~0u;
        bool memo_ok = false;
        while (ts_query_cursor_next_capture(cursor, &m, &ci)) {
            if ((++seen & 511) == 0 && stale()) return false;
            const TSQueryCapture& c = m.captures[ci];
            const u8 style = qs.capture_style[c.index];
            if (style == kSkip) continue;
            const Pattern& pat = qs.patterns[m.pattern_index];
            if (pat.disabled) continue;
            if (!pat.preds.empty()) {
                if (m.id != memo_id || m.pattern_index != memo_pattern) {
                    memo_id = m.id;
                    memo_pattern = m.pattern_index;
                    memo_ok = accept(pat, m);
                }
                if (!memo_ok) continue;
            }
            const u32 s = std::max(ts_node_start_byte(c.node), lo), e = std::min(ts_node_end_byte(c.node), hi);
            if (e <= s) continue;
            caps.push_back({s, e, qs.pattern_base + m.pattern_index, style, static_cast<u8>(pat.priority)});
            mixed = mixed || pat.priority != 100;
        }
    }
    // Later captures paint over earlier ones: by priority,
    // then by where the node starts (so inner nodes win over outer ones),
    // then by where the pattern sits in the query file (later wins). One
    // query delivers its captures in that order already; slices need merging.
    if (sets->size() > 1 || mixed)
        std::stable_sort(caps.begin(), caps.end(), [](const Cap& a, const Cap& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            if (a.start != b.start) return a.start < b.start;
            return a.pattern < b.pattern;
        });
    return true;
}

// Repaints [lo, hi) of the kept range from the gathered captures.
void Syntax::Engine::paint_caps(u32 lo, u32 hi) {
    std::memset(&paint[lo - paint_lo], 0, hi - lo);
    for (const Cap& c : caps) {
        const u32 s = std::max(c.start, lo), e = std::min(c.end, hi);
        if (e > s) std::memset(&paint[s - paint_lo], c.style, e - s);
    }
}

// Keeps the painted range lined up with the text across an edit.
void Syntax::Engine::shift_paint(const TextEdit& ed) {
    if (!paint_valid) return;
    const u64 start = ed.start, old_len = ed.old_end - ed.start, new_len = ed.new_end - ed.start;
    if (start >= paint_hi) return;
    if (start + old_len <= paint_lo) {
        paint_lo = static_cast<u32>(paint_lo + new_len - old_len);
        paint_hi = static_cast<u32>(paint_hi + new_len - old_len);
        return;
    }
    if (start < paint_lo || start + old_len > paint_hi) {
        paint_valid = false;
        return;
    }
    const size_t at = static_cast<size_t>(start - paint_lo);
    const u8 fill = at > 0 ? paint[at - 1] : 0;
    paint.erase(paint.begin() + static_cast<ptrdiff_t>(at), paint.begin() + static_cast<ptrdiff_t>(at + old_len));
    paint.insert(paint.begin() + static_cast<ptrdiff_t>(at), static_cast<size_t>(new_len), fill);
    paint_hi = static_cast<u32>(paint_hi + new_len - old_len);
}

// The painted range as style runs.
void Syntax::Engine::marks(Highlights* out) const {
    out->clear();
    const u32 n = paint_hi - paint_lo;
    if (n == 0) return;
    out->base = paint_lo;
    out->end = paint_hi;
    const u8* p = paint.data();
    u8 cur = p[0];
    out->off.push_back(0);
    out->style.push_back(cur);
    for (u32 i = 1; i < n; ++i) {
        if (p[i] != cur) {
            cur = p[i];
            out->off.push_back(i);
            out->style.push_back(cur);
        }
    }
    out->seal();
}

// ---------------------------------------------------------------- shape

namespace {

// What kind of definition a node of this type is, as a language server's
// symbols are numbered (0: none). By the words grammars use in their node
// types: function_definition, class_specifier, impl_item, mod_item...
u8 definition_kind(std::string_view t) {
    static constexpr std::string_view kNot[] = {"call", "declarator", "parameter", "pointer", "signature", "reference", "expression", "argument", "_list", "identifier", "type_", "body"};
    for (const std::string_view x : kNot)
        if (t.find(x) != std::string_view::npos) return 0;
    auto has = [&](std::string_view x) { return t.find(x) != std::string_view::npos; };
    if (has("namespace")) return 3;
    if (has("module") || t == "mod_item") return 2;
    if (has("constructor")) return 9;
    if (has("interface") || has("trait")) return 11;
    if (has("enum")) return 10;
    if (has("struct") || has("union")) return 23;
    if (has("class") || has("impl")) return 5;
    if (has("method")) return 6;
    if (has("function")) return 12;
    return 0;
}

// The last line a node has anything on (one that ends at the start of a
// line ends on the line before).
u32 last_row(TSNode n) {
    const TSPoint p = ts_node_end_point(n);
    return p.column == 0 && p.row > 0 ? p.row - 1 : p.row;
}

}  // namespace

const Syntax::Engine::Kind& Syntax::Engine::kind_of(TSNode n) {
    const TSSymbol sym = ts_node_symbol(n);
    if (sym >= kinds.size()) kinds.resize(static_cast<size_t>(sym) + 1);
    Kind& k = kinds[sym];
    if (k.known) return k;
    k.known = true;
    const std::string_view type = ts_node_type(n);
    if (!ts_node_is_named(n)) {
        if (type.size() == 1 && std::string_view("()[]{}").find(type[0]) != std::string_view::npos) k.bracket = type[0];
        return k;
    }
    k.definition = definition_kind(type);
    k.fold = type.find("comment") != std::string_view::npos ? 1 : (type == "block" || type.ends_with("_block")) ? 2 : type.starts_with("preproc_if") ? 3 : 0;
    return k;
}

// The shape of bytes [lo, hi), and what encloses them: one walk down the
// tree, into the nodes that reach into the range. Brackets pair up among
// the children of a node (an opening one waits for the next closing one of
// its kind), each as deep as the pairs it is inside; a pair over three
// lines or more, an indented block, a comment of several lines and a #if
// fold; a node whose type names a definition and that has a body is one,
// named by its name (or its declarator's, or its type's).
void Syntax::Engine::shape(u32 lo, u32 hi, SyntaxStructure* out, bool paint_brackets) {
    out->clear();
    if (!tree) return;
    TSTreeCursor c = ts_tree_cursor_new(ts_tree_root_node(tree));
    std::string buf;
    struct Open {
        u32 at, row, depth;
        char ch;
    };
    auto paint_one = [&](u32 at, u32 depth) {
        const u8 style = bracket_style[depth % kBracketDepths];
        if (paint_brackets && style && at >= lo && at < hi && at >= paint_lo && at < paint_hi) paint[at - paint_lo] = style;
    };
    auto in_range = [&](u32 at) { return at >= lo && at < hi; };
    auto closer = [](char ch) { return ch == '(' ? ')' : ch == '[' ? ']' : '}'; };
    auto pair = [&](u32 at, u32 row, u32 close, u32 close_row, u32 depth) {
        paint_one(at, depth);
        paint_one(close, depth);
        if (in_range(at) || in_range(close)) out->pairs.push_back({at, close});
        if (close_row > row + 1) out->folds.push_back({row, close_row - 1});
    };
    // The children of the node the cursor is on (`visit`), and one named
    // child among them (`one`): its folds, whether it is a definition, and
    // its own children.
    auto visit = [&](auto&& self, auto&& one, u32 depth, u32 parent_row, u32 scope_depth, u8 scope_kind) -> void {
        const TSNode parent = ts_tree_cursor_current_node(&c);
        // A node of many children (a file, a class's or a namespace's body)
        // is a list, with brackets at most first and last: its pair is taken
        // from its ends, and of its children only those that reach into the
        // range are looked at.
        if (ts_node_child_count(parent) > 32) {
            if (!ts_tree_cursor_goto_first_child(&c)) return;
            const TSNode first = ts_tree_cursor_current_node(&c);
            const char ch = kind_of(first).bracket;
            ts_tree_cursor_goto_parent(&c);
            const u32 end = ts_node_end_byte(parent);
            u32 len = 0;
            const char* last = end > 0 ? snap.read(end - 1, &len) : nullptr;
            if ((ch == '(' || ch == '[' || ch == '{') && last && len > 0 && *last == closer(ch)) {
                pair(ts_node_start_byte(first), ts_node_start_point(first).row, end - 1, ts_node_end_point(parent).row, depth);
                ++depth;
            }
            if (ts_tree_cursor_goto_first_child_for_byte(&c, lo) < 0) return;
            do {
                const TSNode n = ts_tree_cursor_current_node(&c);
                if (ts_node_start_byte(n) >= hi) break;
                if (ts_node_is_named(n) && ts_node_end_byte(n) > lo) one(self, one, n, depth, parent_row, scope_depth, scope_kind);
            } while (ts_tree_cursor_goto_next_sibling(&c));
            ts_tree_cursor_goto_parent(&c);
            return;
        }
        if (!ts_tree_cursor_goto_first_child(&c)) return;
        std::vector<Open> opens;
        do {
            const TSNode n = ts_tree_cursor_current_node(&c);
            const u32 s = ts_node_start_byte(n);
            if (!ts_node_is_named(n)) {
                const char ch = kind_of(n).bracket;
                if (ch == '(' || ch == '[' || ch == '{') {
                    const u32 d = depth + static_cast<u32>(opens.size());
                    opens.push_back({s, ts_node_start_point(n).row, d, ch});
                    paint_one(s, d);
                } else if (ch) {
                    for (size_t k = opens.size(); k-- > 0;) {
                        if (closer(opens[k].ch) != ch) continue;
                        const Open o = opens[k];
                        opens.resize(k);
                        pair(o.at, o.row, s, ts_node_start_point(n).row, o.depth);
                        break;
                    }
                }
                continue;
            }
            if (s >= hi && opens.empty()) break;  // (past the range, with nothing left to close)
            if (ts_node_end_byte(n) <= lo || s >= hi) continue;  // (beside the range: its brackets are its own)
            one(self, one, n, depth + static_cast<u32>(opens.size()), parent_row, scope_depth, scope_kind);
        } while (ts_tree_cursor_goto_next_sibling(&c));
        ts_tree_cursor_goto_parent(&c);
    };
    auto one = [&](auto&& visit_, auto&& self, TSNode n, u32 depth, u32 parent_row, u32 scope_depth, u8 scope_kind) -> void {
        const Kind& k = kind_of(n);
        const u32 row = ts_node_start_point(n).row;
        if (k.fold) {
            const u32 end_row = last_row(n);
            if (k.fold == 1 && end_row > row) {
                out->folds.push_back({row, end_row});
            } else if (k.fold == 3 && end_row > row + 1) {
                out->folds.push_back({row, end_row - 1});  // (the #endif stays)
            } else if (k.fold == 2 && end_row > parent_row) {
                const TSNode first = ts_node_child(n, 0);
                if (ts_node_is_null(first) || kind_of(first).bracket != '{') out->folds.push_back({parent_row, end_row});  // (an indented block; one in braces folds by them)
            }
        }
        u32 inner_depth = scope_depth;
        u8 inner_kind = scope_kind;
        if (k.definition && !ts_node_is_null(ts_node_child_by_field_name(n, "body", 4))) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            if (ts_node_is_null(name)) {
                TSNode d = n;
                for (TSNode x; !ts_node_is_null(x = ts_node_child_by_field_name(d, "declarator", 10));) d = x;
                if (!ts_node_eq(d, n)) name = d;
            }
            if (ts_node_is_null(name)) name = ts_node_child_by_field_name(n, "type", 4);
            if (!ts_node_is_null(name)) {
                std::string called(node_text(name, &buf, 160));
                for (char& ch : called)
                    if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
                u8 kind = k.definition;
                if (kind == 12 && (scope_kind == 5 || scope_kind == 23 || scope_kind == 11)) kind = 6;  // (a function in a class is its method)
                out->scopes.push_back({std::move(called), kind, scope_depth, ts_node_start_byte(n), ts_node_end_byte(n), ts_node_start_byte(name)});
                inner_depth = scope_depth + 1;
                inner_kind = kind;
            }
        }
        visit_(visit_, self, depth, row, inner_depth, inner_kind);
    };
    visit(visit, one, 0, 0, 0, 0);
    ts_tree_cursor_delete(&c);
    // At most one fold a line, the longest; and in order.
    std::ranges::sort(out->folds, [](const SyntaxFold& a, const SyntaxFold& b) { return a.header != b.header ? a.header < b.header : a.last > b.last; });
    out->folds.erase(std::ranges::unique(out->folds, [](const SyntaxFold& a, const SyntaxFold& b) { return a.header == b.header; }).begin(), out->folds.end());
    std::ranges::sort(out->pairs);
}

void SyntaxStructure::edit(const TextEdit& e) {
    const u64 start = e.start, old_end = e.old_end;
    const u64 d = e.new_end - e.old_end;  // (as two's complement: added, it moves either way)
    size_t keep = 0;
    for (auto& [o, cl] : pairs) {
        if (cl < start) {
        } else if (o >= old_end) {
            o += d, cl += d;
        } else if (o < start && cl >= old_end) {
            cl += d;
        } else {
            continue;  // (an end of it was in what was cut)
        }
        pairs[keep++] = {o, cl};
    }
    pairs.resize(keep);
    const i64 rows = static_cast<i64>(e.new_end_row) - static_cast<i64>(e.old_end_row);
    keep = 0;
    for (SyntaxFold f : folds) {
        if (e.start_row > f.last) {
        } else if (e.old_end_row < f.header) {
            f.header = static_cast<u32>(f.header + rows), f.last = static_cast<u32>(f.last + rows);
        } else if (e.start_row > f.header && e.old_end_row <= f.last) {  // in what folds
            f.last = static_cast<u32>(f.last + rows);
            if (f.last <= f.header) continue;
        } else if (!(e.start_row == f.header && e.old_end_row == f.header && rows == 0)) {
            continue;  // (its header line was split or joined)
        }
        folds[keep++] = f;
    }
    folds.resize(keep);
    keep = 0;
    for (SyntaxScope& sc : scopes) {
        if (sc.end < start) {
        } else if (sc.start >= old_end) {
            sc.start += d, sc.end += d, sc.name_at += d;
        } else if (sc.start < start && sc.end >= old_end) {
            sc.end += d;
            if (sc.name_at >= old_end) sc.name_at += d;
        } else {
            continue;
        }
        if (&scopes[keep] != &sc) scopes[keep] = std::move(sc);
        ++keep;
    }
    scopes.resize(keep);
}

// ---------------------------------------------------------------- languages

std::string Syntax::language_for(std::string_view path, std::string_view first_line, std::span<const GrammarSource> own) {
    const size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    std::string ext;
    if (dot != std::string_view::npos)
        for (char c : name.substr(dot + 1)) ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // The settings first, so that they can add a language or take an
    // ending from a built-in one.
    if (!ext.empty())
        for (const GrammarSource& g : own)
            if (std::ranges::find(g.extensions, ext) != g.extensions.end()) return g.name;

    // Built in: C, C++ and HIP, Python, and the files around them (build
    // files, settings, notes, scripts). Any other language comes from the
    // settings, as above.
    static const struct {
        const char* ext;
        const char* lang;
    } by_ext[] = {
        {"c", "c"}, {"h", "cpp"}, {"cc", "cpp"}, {"cpp", "cpp"}, {"cxx", "cpp"}, {"hpp", "cpp"}, {"hh", "cpp"},
        {"hxx", "cpp"}, {"ipp", "cpp"}, {"inl", "cpp"}, {"hip", "cpp"},
        {"py", "python"}, {"pyi", "python"}, {"ipynb", "python"},
        {"cmake", "cmake"}, {"mk", "make"}, {"json", "json"}, {"jsonc", "json"}, {"toml", "toml"},
        {"yaml", "yaml"}, {"yml", "yaml"}, {"md", "markdown"}, {"markdown", "markdown"},
        {"sh", "bash"}, {"bash", "bash"}, {"zsh", "bash"}, {"lua", "lua"},
    };
    for (const auto& e : by_ext)
        if (ext == e.ext) return e.lang;

    static const struct {
        const char* name;
        const char* lang;
    } by_name[] = {
        {"CMakeLists.txt", "cmake"}, {"Makefile", "make"}, {"makefile", "make"}, {"GNUmakefile", "make"},
        {".bashrc", "bash"}, {".zshrc", "bash"}, {".zprofile", "bash"}, {".bash_profile", "bash"}, {".profile", "bash"},
    };
    for (const auto& e : by_name)
        if (name == e.name) return e.lang;

    if (first_line.starts_with("#!")) {
        static const struct {
            const char* word;
            const char* lang;
        } by_shebang[] = {{"python", "python"}, {"bash", "bash"}, {"zsh", "bash"}, {"/sh", "bash"}, {"env sh", "bash"}, {"lua", "lua"}};
        for (const auto& e : by_shebang)
            if (first_line.find(e.word) != std::string_view::npos) return e.lang;
    }
    return {};
}

bool Syntax::installed(const std::string& lang, const SyntaxOptions& opt) {
    return !lang.empty() && !find_parser(lang, opt).empty();
}

// A code block's language, by its name, an alias, or a file ending.
std::string Syntax::language_named(std::string_view name, std::span<const GrammarSource> own, const SyntaxOptions& opt) {
    std::string n;
    for (const char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const struct {
        const char* name;
        const char* lang;
    } aliases[] = {
        {"c++", "cpp"}, {"python3", "python"}, {"shell", "bash"},
    };
    std::string lang;
    for (const auto& a : aliases)
        if (n == a.name) lang = a.lang;
    if (lang.empty() && !n.empty() && n.find_first_of("/\\.") == std::string::npos) {
        if (installed(n, opt)) lang = n;
        else if (lang = language_for("x." + n, "", own); lang.empty()) lang = language_for(n, "", own);
    }
    if (lang == "cpp" && !installed(lang, opt)) lang = "c";  // (as a file of it would be)
    return installed(lang, opt) ? lang : std::string();
}

bool Syntax::highlight_text(const std::string& lang, std::string_view text, const SyntaxOptions& opt, std::vector<u8>* styles,
                            std::vector<std::string>* names) {
    if (text.empty() || text.size() > (4u << 20) || !installed(lang, opt)) return false;
    Engine e;
    std::string err;
    e.bundle = get_bundle(lang, opt, &err);
    if (!e.bundle) return false;
    e.parser = ts_parser_new();
    if (!ts_parser_set_language(e.parser, e.bundle->language)) return false;
    const u32 n = static_cast<u32>(text.size());
    e.tree = ts_parser_parse_string(e.parser, nullptr, text.data(), n);
    if (!e.tree) return false;
    e.snap.pieces.push_back({text.data(), n, 0});
    e.snap.index();
    {
        // The whole query, or its slices, whichever is ready first (as Syntax::work() picks).
        Bundle& b = *e.bundle;
        std::unique_lock lk(b.mu);
        const std::vector<QuerySet>* pick = nullptr;
        for (;;) {
            if (b.whole_done && !b.whole.empty()) pick = &b.whole;
            else if (b.parts_done && b.parts_usable && !b.parts.empty()) pick = &b.parts;
            if (pick || (b.whole_done && b.parts_done)) break;
            b.cv.wait_for(lk, std::chrono::milliseconds(2));
        }
        if (!pick) return false;
        *names = b.names;
        e.use(pick);
    }
    if (!e.collect(0, n, [] { return false; })) return false;
    e.paint.resize(n);
    e.paint_lo = 0;
    e.paint_hi = n;
    e.paint_caps(0, n);
    *styles = std::move(e.paint);
    return true;
}

// ---------------------------------------------------------------- main thread

bool Syntax::attach(const Buffer& buf, const std::string& lang, const SyntaxOptions& opt) {
    detach();
    std::string use = lang;
    if (use == "cpp" && !installed(use, opt)) use = "c";  // headers still read well as C
    if (buf.size() > std::min<u64>(opt.max_bytes, 0x7FFFFFFFull) || !installed(use, opt)) return false;
    lang_ = use;
    opt_ = opt;
    active_ = true;
    held_.store(opt.hold_full_parse);
    attached_ns_ = now_ns();
    engine_ = new Engine;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Syntax::detach() {
    if (thread_.joinable()) {
        cancel_.store(true);
        {
            std::lock_guard lk(mu_);
            quit_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    delete engine_;
    engine_ = nullptr;
    active_ = false;
    lang_.clear();
    version_ = 0;
    unsent_.clear();
    recent_.clear();
    sent_any_ = false;
    asked_first_ = asked_last_ = 0;
    hl_.clear();
    names_.clear();
    job_ = Job{};
    result_ = Result{};
    stats_ = SyntaxStats{};
    busy_ = quit_ = false;
    cancel_.store(false);
    pending_.store(false);
    held_.store(false);
}

void Syntax::release() {
    if (!held_.exchange(false)) return;
    std::lock_guard lk(mu_);
    wake_.notify_all();
}

void Syntax::on_edit(const TextEdit& e) {
    if (!active_) return;
    ++version_;
    unsent_.push_back(e);
    recent_.emplace_back(version_, e);
    hl_.edit(e.start, e.old_end - e.start, e.new_end - e.start);  // keep the colours in step meanwhile
    shape_.edit(e);
}

void Syntax::want(const Buffer& buf, u64 first, u64 last) {
    if (!active_) return;
    const u64 lines = buf.line_count();
    last = std::min(last, lines - 1);
    first = std::min(first, last);

    // Ask again when the window is getting close to the edge of what was
    // highlighted, well before it shows.
    const u64 margin = opt_.margin_lines, guard = margin / 4;
    const bool near_top = asked_first_ > 0 && first < asked_first_ + guard;
    const bool near_bottom = asked_last_ + 1 < lines && last + guard > asked_last_;
    const bool reparse = !unsent_.empty() || !sent_any_;
    if (!reparse && !near_top && !near_bottom) return;

    auto line_byte = [&](u64 line) { return line >= lines ? buf.size() : buf.line_start(line); };
    Job j;
    j.valid = true;
    j.reparse = reparse;
    j.version = version_;
    const u64 from = std::saturating_sub(first, margin), to = std::min(lines - 1, std::saturating_add(last, margin));
    j.lo = line_byte(from);
    j.hi = line_byte(to + 1);
    j.vis_lo = line_byte(std::saturating_sub<u64>(first, 4));
    j.vis_hi = line_byte(last + 5);
    if (reparse) {
        buf.snapshot(&j.snap.pieces);
        j.edits = std::move(unsent_);
        unsent_.clear();
    }
    {
        std::lock_guard lk(mu_);
        if (job_.valid && job_.reparse) {  // the worker has not started the previous request: fold it into this one
            job_.edits.insert(job_.edits.end(), j.edits.begin(), j.edits.end());
            j.edits = std::move(job_.edits);
            if (!j.reparse) j.snap = std::move(job_.snap);
            j.reparse = true;
        }
        job_ = std::move(j);
        pending_.store(true);
    }
    wake_.notify_one();
    sent_any_ = true;
    asked_first_ = from;
    asked_last_ = to;
}

bool Syntax::poll() {
    Result r;
    {
        std::lock_guard lk(mu_);
        if (!result_.valid) return false;
        r = std::move(result_);
        result_ = Result{};
    }
    if (!r.message.empty()) message_ = std::move(r.message);
    if (r.failed) {
        active_ = false;
        hl_.clear();
        return true;
    }
    if (r.has_names) names_ = std::move(r.names);
    // Bring the result up to date with edits made after it was computed.
    size_t keep = 0;
    for (size_t i = 0; i < recent_.size(); ++i) {
        if (recent_[i].first <= r.version) continue;
        const TextEdit& e = recent_[i].second;
        r.hl.edit(e.start, e.old_end - e.start, e.new_end - e.start);
        r.shape.edit(e);
        recent_[keep++] = recent_[i];
    }
    recent_.resize(keep);
    // The pass over the whole range usually confirms what the quick pass
    // over the visible lines already showed: then there is nothing to redraw.
    auto same_folds = [](const std::vector<SyntaxFold>& a, const std::vector<SyntaxFold>& b) {
        return std::ranges::equal(a, b, [](const SyntaxFold& x, const SyntaxFold& y) { return x.header == y.header && x.last == y.last; });
    };
    const bool changed = r.has_names || !hl_.same_as(r.hl) || shape_.pairs != r.shape.pairs || !same_folds(shape_.folds, r.shape.folds) ||
                         shape_.scopes.size() != r.shape.scopes.size();
    hl_ = std::move(r.hl);
    shape_ = std::move(r.shape);
    return changed;
}

std::string Syntax::take_message() {
    std::string m = std::move(message_);
    message_.clear();
    return m;
}

void Syntax::wait_idle() {
    if (!thread_.joinable()) return;
    std::unique_lock lk(mu_);
    idle_.wait(lk, [&] { return !job_.valid && !busy_; });
}

bool Syntax::ask(const Buffer& buf, Ask& q, int wait_ms) {
    if (!active_ || !thread_.joinable()) return false;
    if (!unsent_.empty() || !sent_any_) want(buf, asked_first_, asked_last_);  // (the edits first: the answer is of the text as it is)
    q.version = version_;
    {
        std::lock_guard lk(mu_);
        ask_ = Ask{};
        ask_.kind = q.kind;
        ask_.version = q.version;
        ask_.lo = q.lo;
        ask_.hi = q.hi;
    }
    wake_.notify_one();
    std::unique_lock lk(mu_);
    const bool came = idle_.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] { return ask_.answered; });
    const bool ok = came && ask_.ok;
    if (came) {
        q.ranges = std::move(ask_.ranges);
        q.all = std::move(ask_.all);
    }
    ask_ = Ask{};
    return ok;
}

bool Syntax::ask_around(const Buffer& buf, u64 lo, u64 hi, std::vector<std::pair<u64, u64>>* out, int wait_ms) {
    Ask q;
    q.kind = Ask::kAround;
    q.lo = lo;
    q.hi = hi;
    if (!ask(buf, q, wait_ms)) return false;
    *out = std::move(q.ranges);
    return true;
}

bool Syntax::ask_all(const Buffer& buf, SyntaxStructure* out, int wait_ms) {
    Ask q;
    q.kind = Ask::kAll;
    if (!ask(buf, q, wait_ms)) return false;
    *out = std::move(q.all);
    return true;
}

// On the worker: a question, of the tree as it is (which is of the text it
// was asked about: the edits before it were sent first).
void Syntax::answer(Ask& q) {
    Engine& e = *engine_;
    q.ok = false;
    if (!e.tree || tree_version_ != q.version) return;
    const u32 size = e.snap.size;
    if (q.kind == Ask::kAll) {
        e.shape(0, size, &q.all, false);
        q.ok = true;
        return;
    }
    // The nodes around the range, each larger than the last; a node in
    // brackets offers what is between them first.
    const u32 lo = static_cast<u32>(std::min<u64>(q.lo, size)), hi = static_cast<u32>(std::min<u64>(q.hi, size));
    u64 have_lo = lo, have_hi = hi;
    auto offer = [&](u64 a, u64 b) {
        if (a <= have_lo && b >= have_hi && (a < have_lo || b > have_hi)) {
            q.ranges.emplace_back(a, b);
            have_lo = a, have_hi = b;
        }
    };
    for (TSNode n = ts_node_descendant_for_byte_range(ts_tree_root_node(e.tree), lo, hi); !ts_node_is_null(n); n = ts_node_parent(n)) {
        const u32 count = ts_node_child_count(n);
        if (count >= 2) {
            const TSNode first = ts_node_child(n, 0), last = ts_node_child(n, count - 1);
            const std::string_view a = ts_node_type(first), b = ts_node_type(last);
            if (!ts_node_is_named(first) && !ts_node_is_named(last) &&
                ((a == "(" && b == ")") || (a == "[" && b == "]") || (a == "{" && b == "}") || (a == "\"" && b == "\"")))
                offer(ts_node_end_byte(first), ts_node_start_byte(last));
        }
        offer(ts_node_start_byte(n), ts_node_end_byte(n));
    }
    q.ok = true;
}

SyntaxStats Syntax::stats() {
    std::lock_guard lk(mu_);
    return stats_;
}

// ---------------------------------------------------------------- worker thread

void Syntax::run() {
    thread_soon(Soon::kWaitedFor);
    for (;;) {
        Job job;
        Ask q;
        {
            std::unique_lock lk(mu_);
            wake_.wait(lk, [&] { return job_.valid || (ask_.kind != Ask::kNone && !ask_.answered) || quit_; });
            if (quit_) return;
            if (job_.valid) {  // (a question waits for the text before it to be parsed)
                job = std::move(job_);
                job_ = Job{};
                pending_.store(false);
            } else {
                q.kind = ask_.kind;
                q.version = ask_.version;
                q.lo = ask_.lo;
                q.hi = ask_.hi;
            }
            busy_ = true;
        }
        if (job.valid) work(job);
        else answer(q);
        {
            std::lock_guard lk(mu_);
            busy_ = false;
            if (job.valid) ++stats_.jobs;
            else if (ask_.kind == q.kind && ask_.version == q.version && !ask_.answered) {
                ask_.ranges = std::move(q.ranges);
                ask_.all = std::move(q.all);
                ask_.ok = q.ok;
                ask_.answered = true;
            }
        }
        idle_.notify_all();
    }
}

void Syntax::publish(Result&& r) {
    {
        std::lock_guard lk(mu_);
        if (result_.valid && result_.has_names && !r.has_names) {  // an uncollected result carried the names
            r.names = std::move(result_.names);
            r.has_names = true;
        }
        if (result_.valid && r.message.empty()) r.message = std::move(result_.message);
        r.valid = true;
        result_ = std::move(r);
    }
    if (notify_) notify_();
}

void Syntax::work(Job& job) {
    Engine& e = *engine_;
    if (e.failed) return;  // nothing will work; the editor turns highlighting off
    auto fail = [&](const std::string& why) {
        e.failed = true;
        Result r;
        r.failed = true;
        r.message = why;
        publish(std::move(r));
    };
    auto since = [](u64 t0) { return static_cast<double>(now_ns() - t0) * 1e-6; };
    auto gone = [&] { return cancel_.load(std::memory_order_relaxed); };
    auto superseded = [&] { return gone() || pending_.load(std::memory_order_relaxed); };

    // First request: find the grammar. Its highlight query starts compiling
    // on other threads right away, while this one parses.
    if (!e.bundle) {
        std::string err;
        e.bundle = get_bundle(lang_, opt_, &err);
        if (!e.bundle) return fail(err);
        e.parser = ts_parser_new();
        if (!ts_parser_set_language(e.parser, e.bundle->language))
            return fail("grammar for " + lang_ + " does not match this tree-sitter version");
    }

    // Parses the snapshot, reusing `old` where the text did not change.
    // Null means the editor let go of the document meanwhile.
    auto parse = [&](TSTree* old) {
        TSInput input;
        input.payload = &e.snap;
        input.read = [](void* payload, uint32_t byte, TSPoint, uint32_t* n) {
            return static_cast<const Snapshot*>(payload)->read(byte, n);
        };
        input.encoding = TSInputEncodingUTF8;
        input.decode = nullptr;
        TSParseOptions po;
        po.payload = this;
        po.progress_callback = [](TSParseState* s) {
            return static_cast<Syntax*>(s->payload)->cancel_.load(std::memory_order_relaxed);
        };
        TSTree* t = ts_parser_parse_with_options(e.parser, old, input, po);
        if (!t) ts_parser_reset(e.parser);
        return t;
    };
    auto forget_walk = [&] {
        if (e.walking) ts_tree_cursor_delete(&e.walk);
        e.walking = false;
    };

    // Picks the compiled query: the whole one if it is ready, its slices if
    // they are, else waits for whichever comes first. False: failed or gone.
    auto choose = [&] {
        Bundle& b = *e.bundle;
        std::unique_lock lk(b.mu);
        const std::vector<QuerySet>* pick = nullptr;
        for (;;) {
            if (b.whole_done && !b.whole.empty()) pick = &b.whole;
            else if (b.parts_done && b.parts_usable && !b.parts.empty()) pick = &b.parts;
            if (pick || (b.whole_done && b.parts_done)) break;
            b.cv.wait_for(lk, std::chrono::milliseconds(2));
            if (gone()) return false;
        }
        if (!pick) {
            const std::string why = b.error.empty() ? "no usable highlight query for " + lang_ : b.error;
            lk.unlock();
            fail(why);
            return false;
        }
        if (pick != e.sets || !e.names_sent) {
            for (u32 k = 0; k < kBracketDepths; ++k) {
                const auto it = std::ranges::find(b.names, std::string_view(kBracketStyles[k]));
                e.bracket_style[k] = it == b.names.end() ? 0 : static_cast<u8>(it - b.names.begin());
            }
        }
        if (pick != e.sets) {
            const bool first = e.sets == nullptr;
            e.use(pick);
            e.paint_valid = false;  // repaint with the query now in use
            e.names_sent = false;
            if (first) {
                std::lock_guard lk2(mu_);
                stats_.setup_ms = since(attached_ns_);
            }
        }
        return true;
    };

    auto send = [&] {
        Result out;
        out.version = job.version;
        e.marks(&out.hl);
        out.shape = std::move(e.shaped);
        if (!e.names_sent) {
            std::lock_guard lk(e.bundle->mu);
            out.names = e.bundle->names;
            out.has_names = true;
            e.names_sent = true;
        }
        {
            std::lock_guard lk(mu_);
            stats_.marks = out.hl.size();
            stats_.parts = static_cast<u32>(e.sets->size());
        }
        publish(std::move(out));
    };
    // Highlights all of [lo, hi) and makes it the kept range.
    auto full_pass = [&](u32 lo, u32 hi, auto&& stale) {
        if (hi <= lo) {
            e.paint_valid = false;
            e.paint_lo = e.paint_hi = lo;
            return true;
        }
        if (!e.collect(lo, hi, stale)) return false;
        e.paint.resize(hi - lo);
        e.paint_lo = lo;
        e.paint_hi = hi;
        e.paint_valid = true;
        e.paint_caps(lo, hi);
        e.shape(lo, hi, &e.shaped, true);
        return true;
    };

    if (job.reparse) {
        if (e.tree) {
            for (const TextEdit& ed : job.edits) {
                TSInputEdit ie;
                ie.start_byte = static_cast<u32>(ed.start);
                ie.old_end_byte = static_cast<u32>(ed.old_end);
                ie.new_end_byte = static_cast<u32>(ed.new_end);
                ie.start_point = {ed.start_row, ed.start_col};
                ie.old_end_point = {ed.old_end_row, ed.old_end_col};
                ie.new_end_point = {ed.new_end_row, ed.new_end_col};
                ts_tree_edit(e.tree, &ie);
                e.shift_paint(ed);
            }
        }
        e.snap = std::move(job.snap);
        e.snap.index();

        // A big file takes a while to parse (about 13 MB a second). When the
        // window is near its start, parse just that much first: text up to a
        // point parses the same whatever follows, so the first screen gets
        // its colours at once and the full parse follows.
        const u32 full = e.snap.size;
        const u64 quick_end = job.hi + (64u << 10);
        if (!e.tree && full > (256u << 10) && quick_end < full / 2) {
            const u64 t0 = now_ns();
            e.snap.size = static_cast<u32>(quick_end);
            TSTree* quick = parse(nullptr);
            e.snap.size = full;
            if (!quick) return;
            e.tree = quick;
            const bool shown = choose() && full_pass(static_cast<u32>(std::min<u64>(job.lo, quick_end)),
                                                     static_cast<u32>(std::min<u64>(job.hi, quick_end)), gone);
            if (shown) send();
            forget_walk();
            ts_tree_delete(quick);
            e.tree = nullptr;
            if (!shown) return;
            std::unique_lock lk(mu_);
            stats_.quick_ms = since(t0);
            // The first screen is coloured. If asked to, let the caller get
            // its window up before starting the long parse.
            wake_.wait(lk, [&] { return !held_.load() || quit_; });
            if (quit_) return;
        }

        const u64 t0 = now_ns();
        TSTree* next = parse(e.tree);
        if (!next) return;
        if (e.tree) ts_tree_delete(e.tree);
        e.tree = next;
        tree_version_ = job.version;
        forget_walk();
        std::lock_guard lk(mu_);
        stats_.parse_ms = since(t0);
    }
    if (!e.tree || !choose()) return;

    const u32 size = e.snap.size;
    const u32 lo = static_cast<u32>(std::min<u64>(job.lo, size)), hi = static_cast<u32>(std::min<u64>(job.hi, size));
    const u32 vlo = static_cast<u32>(std::min<u64>(job.vis_lo, size)), vhi = static_cast<u32>(std::min<u64>(job.vis_hi, size));

    // After an edit, the lines on screen first: a small pass, sent at once.
    if (job.reparse && e.paint_valid && vlo >= e.paint_lo && vhi <= e.paint_hi && vhi > vlo) {
        const u64 t0 = now_ns();
        if (!e.collect(vlo, vhi, gone)) return;
        e.paint_caps(vlo, vhi);
        e.shape(vlo, vhi, &e.shaped, true);
        send();
        std::lock_guard lk(mu_);
        stats_.visible_ms = since(t0);
    }
    // Then the whole range, unless something newer is already waiting.
    const u64 t0 = now_ns();
    if (!full_pass(lo, hi, superseded)) return;
    send();
    std::lock_guard lk(mu_);
    stats_.query_ms = since(t0);
}

}  // namespace blit
