#include "buildread.hpp"

#include "json.hpp"

namespace blit {

namespace {

constexpr size_t kMaxMessage = 600;  // bytes of a message kept (as BuildScan's)
constexpr size_t kMaxNotes = 32;     // notes a problem keeps (an analyzer's path can be long)

// A place to read files from: "ssh://host" (or none) and a folder there.
struct Where {
    std::string prefix, dir;
};

Where where_of(const std::string& path) {
    Where w;
    std::string p = path;
    if (p.starts_with("ssh://") || p.starts_with("docker://")) {
        const size_t slash = p.find('/', p.find("://") + 3);
        w.prefix = p.substr(0, slash);
        p = slash == std::string::npos ? "/" : p.substr(slash);
    }
    const size_t slash = p.rfind('/');
    w.dir = slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
    return w;
}

std::string message_of(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    return std::string(s.substr(0, kMaxMessage));
}

// A URI's path: "file:///a%20b/c" is "/a b/c"; a relative one is as it is.
std::string uri_path(std::string_view uri) {
    if (uri.starts_with("file://")) {
        uri.remove_prefix(7);
        uri.remove_prefix(std::min(uri.size(), uri.find('/')));  // (a host, if any)
    }
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            out += static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------- SARIF

struct Sarif {
    const Where& w;
    const std::function<bool(const std::string&)>& exists;
    json::Value run;

    // A path as a file here (with the prefix): a whole one as it is, a
    // relative one from `base`.
    std::string full(const std::string& p, const std::string& base) const {
        if (!p.empty() && p[0] == '/') return w.prefix + tidy_path(p);
        return w.prefix + tidy_path(base + "/" + p);
    }
    // The folder a uriBaseId stands for, from originalUriBaseIds (which may
    // name another), as a path; "" when it says none.
    std::string base(std::string_view id, int depth = 0) const {
        const json::Value b = run["originalUriBaseIds"][id];
        if (!b || depth > 4) return {};
        std::string p = uri_path(b["uri"].str());
        if (!b["uriBaseId"].str().empty()) p = base(b["uriBaseId"].str(), depth + 1) + "/" + p;
        return p.empty() || p[0] != '/' ? std::string() : p;
    }
    // An artifactLocation's file: its uri (or its artifact's, by index),
    // from its base where the file is there, else from the folder of the
    // file read.
    std::string file(json::Value loc) const {
        std::string_view uri = loc["uri"].str(), id = loc["uriBaseId"].str();
        if (uri.empty() && loc["index"].is_number()) {
            const json::Value a = run["artifacts"].at(static_cast<size_t>(loc["index"].integer()))["location"];
            uri = a["uri"].str();
            if (id.empty()) id = a["uriBaseId"].str();
        }
        if (uri.empty()) return {};
        const std::string p = uri_path(uri);
        if (!p.empty() && p[0] == '/') return full(p, w.dir);
        if (!id.empty())
            if (const std::string b = base(id); !b.empty()) {
                const std::string there = full(p, b);
                if (!exists || exists(there)) return there;
            }
        return full(p, w.dir);
    }
    // A location's place; false if it has none.
    bool place(json::Value loc, std::string* path, u32* line, u32* col) const {
        const json::Value phys = loc["physicalLocation"];
        *path = file(phys["artifactLocation"]);
        if (path->empty()) return false;
        const json::Value r = phys["region"];
        *line = static_cast<u32>(std::max<i64>(1, r["startLine"].integer(1)));
        *col = static_cast<u32>(std::max<i64>(0, r["startColumn"].integer(0)));
        return true;
    }
    // A message: its text (or markdown), its {0} {1} filled from its
    // arguments; else the rule's message of that id, or its description.
    std::string text(json::Value m, json::Value rule) const {
        std::string_view t = m["text"].str();
        if (t.empty()) t = m["markdown"].str();
        if (t.empty() && !m["id"].str().empty()) t = rule["messageStrings"][m["id"].str()]["text"].str();
        if (t.empty()) t = rule["shortDescription"]["text"].str();
        std::string out;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '{' && i + 2 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9' && t[i + 2] == '}') {
                out += m["arguments"].at(static_cast<size_t>(t[i + 1] - '0')).str();
                i += 2;
            } else {
                out += t[i];
            }
        }
        return message_of(out);
    }
};

void read_sarif(const json::Value root, const Where& w, const std::function<bool(const std::string&)>& exists, std::vector<BuildProblem>* out) {
    for (const json::Value run : root["runs"]) {
        const Sarif s{w, exists, run};
        const json::Value rules = run["tool"]["driver"]["rules"];
        for (const json::Value r : run["results"]) {
            if (out->size() >= BuildScan::kMaxProblems) return;
            const std::string_view kind = r["kind"].str();
            if (kind == "pass" || kind == "notApplicable" || kind == "informational") continue;
            std::string_view code = r["ruleId"].str();
            json::Value rule;
            if (r["ruleIndex"].is_number()) rule = rules.at(static_cast<size_t>(r["ruleIndex"].integer()));
            else
                for (const json::Value x : rules)
                    if (!code.empty() && x["id"].str() == code) rule = x;
            if (code.empty()) code = rule["id"].str();
            std::string_view level = r["level"].str();
            if (level.empty()) level = rule["defaultConfiguration"]["level"].str();
            if (level.empty()) level = "warning";  // (SARIF's own default)
            if (level == "none") continue;
            BuildNote at;
            if (!s.place(r["locations"].at(0), &at.path, &at.line, &at.col)) continue;  // (no place: as a build's line with none)
            at.message = s.text(r["message"], rule);
            if (level == "note") {  // (GCC writes a note as a result of its own, after the one it is about)
                if (!out->empty() && out->back().notes.size() < kMaxNotes) out->back().notes.push_back(std::move(at));
                continue;
            }
            BuildProblem p;
            p.path = std::move(at.path);
            p.line = at.line;
            p.col = at.col;
            p.severity = level == "error" ? 1 : 2;
            p.message = std::move(at.message);
            if (code != "error" && code != "warning" && code != "note") p.code = code;  // (GCC's for a diagnostic with no option of its own)
            auto note = [&](json::Value loc, std::string message) {
                BuildNote n;
                if (p.notes.size() >= kMaxNotes || !s.place(loc, &n.path, &n.line, &n.col)) return;
                if (n.path == p.path && n.line == p.line && n.col == p.col && message == p.message) return;  // (an analyzer's path ends at the problem)
                n.message = std::move(message);
                p.notes.push_back(std::move(n));
            };
            for (const json::Value rel : r["relatedLocations"]) {
                std::string m = s.text(rel["message"], {});
                for (const json::Value k : rel["relationships"].at(0)["kinds"])
                    if (m.empty() && k.str() == "includes") m = "included here";  // (GCC's include chain)
                note(rel, std::move(m));
            }
            for (const json::Value flow : r["codeFlows"])
                for (const json::Value thread : flow["threadFlows"])
                    for (const json::Value step : thread["locations"]) note(step["location"], s.text(step["location"]["message"], {}));
            out->push_back(std::move(p));
        }
    }
}

// ---------------------------------------------------------------- GCC's JSON

bool gcc_place(json::Value loc, const Where& w, std::string* path, u32* line, u32* col) {
    const json::Value caret = loc["caret"];
    const std::string_view f = caret["file"].str();
    if (f.empty()) return false;
    *path = w.prefix + tidy_path(f[0] == '/' ? std::string(f) : w.dir + "/" + std::string(f));
    *line = static_cast<u32>(std::max<i64>(1, caret["line"].integer(1)));
    *col = static_cast<u32>(std::max<i64>(0, caret["display-column"].integer(caret["column"].integer(0))));
    return true;
}

void read_gcc(const json::Value root, const Where& w, std::vector<BuildProblem>* out) {
    for (const json::Value d : root) {
        if (out->size() >= BuildScan::kMaxProblems) return;
        const std::string_view kind = d["kind"].str();
        BuildNote at;
        if (!gcc_place(d["locations"].at(0), w, &at.path, &at.line, &at.col)) continue;
        at.message = message_of(d["message"].str());
        if (kind == "note") {  // (one after the problem it is about, not among its children)
            if (!out->empty() && out->back().notes.size() < kMaxNotes) out->back().notes.push_back(std::move(at));
            continue;
        }
        BuildProblem p;
        p.path = std::move(at.path);
        p.line = at.line;
        p.col = at.col;
        p.severity = kind == "warning" ? 2 : 1;  // (error, fatal error, sorry, ice)
        p.message = std::move(at.message);
        p.code = d["option"].str();
        for (const json::Value c : d["children"]) {
            BuildNote n;
            if (p.notes.size() >= kMaxNotes || !gcc_place(c["locations"].at(0), w, &n.path, &n.line, &n.col)) continue;
            n.message = message_of(c["message"].str());
            p.notes.push_back(std::move(n));
        }
        out->push_back(std::move(p));
    }
}

// The JSON values one after another in text, each starting a line (what
// is between them is left out).
std::vector<std::string_view> json_values(std::string_view text) {
    std::vector<std::string_view> out;
    for (size_t i = 0; i < text.size();) {
        if ((text[i] != '[' && text[i] != '{') || (i > 0 && text[i - 1] != '\n')) {
            ++i;
            continue;
        }
        size_t depth = 0, j = i;
        bool quoted = false;
        for (; j < text.size(); ++j) {
            const char c = text[j];
            if (quoted) {
                if (c == '\\') ++j;
                else if (c == '"') quoted = false;
            } else if (c == '"') {
                quoted = true;
            } else if (c == '[' || c == '{') {
                ++depth;
            } else if ((c == ']' || c == '}') && --depth == 0) {
                break;
            }
        }
        if (j >= text.size()) break;
        out.push_back(text.substr(i, j + 1 - i));
        i = j + 1;
    }
    return out;
}

}  // namespace

bool read_problems(std::string_view text, const std::string& path, const std::function<bool(const std::string&)>& exists, std::vector<BuildProblem>* out,
                   std::string* kind) {
    const Where w = where_of(path);
    const size_t first = text.find_first_not_of(" \t\r\n");
    const char c = first == std::string_view::npos ? '\0' : text[first];
    // JSON: SARIF, or GCC's (a build's log of it has an array for each file
    // compiled, one after another, make's lines between them).
    json::Doc doc;
    const bool whole = (c == '{' || c == '[') && doc.parse(std::string(text));
    bool any = false;
    auto take = [&](const json::Value root) {
        if (root.is_object() && root["runs"].is_array()) {
            read_sarif(root, w, exists, out);
            *kind = "SARIF";
            any = true;
        } else if (root.is_array() && (root.at(0)["kind"].is_string() || (whole && root.size() == 0))) {
            read_gcc(root, w, out);
            *kind = "GCC's JSON";
            any = true;
        }
    };
    if (whole) take(doc.root());
    else
        for (const std::string_view d : json_values(text))
            if (doc.parse(std::string(d))) take(doc.root());
    if (any) return true;
    if (whole) return *kind = "neither SARIF nor GCC's JSON diagnostics", false;
    // A log: as a build's output, from the file's folder.
    BuildScan scan(w.prefix + w.dir, exists);
    scan.feed(text);
    scan.finish();
    *out = std::move(scan.found());
    *kind = "a log";
    return true;
}

}  // namespace blit
