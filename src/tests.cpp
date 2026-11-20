#include "tests.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "json.hpp"

namespace blit {

namespace {

std::string last_part(std::string_view id, std::string_view sep) {
    const size_t at = id.rfind(sep);
    return std::string(at == std::string_view::npos ? id : id.substr(at + sep.size()));
}

std::string unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        static constexpr std::pair<std::string_view, char> kNamed[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool done = false;
        for (const auto& [name, c] : kNamed)
            if (s.substr(i, name.size()) == name) {
                out += c;
                i += name.size() - 1;
                done = true;
                break;
            }
        if (!done && s.substr(i, 2) == "&#") {
            const size_t semi = s.find(';', i);
            if (semi != std::string_view::npos) {
                const std::string num(s.substr(i + 2, semi - i - 2));
                const long cp = num.starts_with("x") ? std::strtol(num.c_str() + 1, nullptr, 16) : std::strtol(num.c_str(), nullptr, 10);
                if (cp > 0 && cp < 0x80) out += static_cast<char>(cp);
                i = semi;
                done = true;
            }
        }
        if (!done) out += '&';
    }
    return out;
}

// An attribute of an element whose text is `tag` (from its "<" to its ">").
std::string attribute(std::string_view tag, std::string_view name) {
    for (size_t at = 0; (at = tag.find(name, at)) != std::string_view::npos; at += name.size()) {
        if (at == 0 || !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;
        size_t i = at + name.size();
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || tag[i] != '=') continue;
        ++i;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) continue;
        const char q = tag[i];
        const size_t end = tag.find(q, i + 1);
        if (end == std::string_view::npos) return {};
        return unescape(tag.substr(i + 1, end - i - 1));
    }
    return {};
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

std::vector<TestCase> parse_ctest_json(std::string_view text, const std::string& dir) {
    std::vector<TestCase> out;
    json::Doc doc;
    if (!doc.parse(std::string(text))) return out;
    for (const json::Value t : doc.root()["tests"]) {
        TestCase c;
        c.runner = TestCase::kCTest;
        c.id = std::string(t["name"].str());
        if (c.id.empty()) continue;
        c.name = c.id;
        c.dir = dir;
        c.group = last_part(dir, "/");
        for (const json::Value w : t["command"]) c.command.emplace_back(w.str());
        for (const json::Value p : t["properties"])
            if (p["name"].str() == "WORKING_DIRECTORY") c.cwd = std::string(p["value"].str());
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<TestCase> parse_pytest_collect(std::string_view text) {
    std::vector<TestCase> out;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const size_t sep = line.find("::");
        if (sep == std::string_view::npos || line.find(' ') != std::string_view::npos) continue;  // (the summary, a warning)
        TestCase c;
        c.runner = TestCase::kPytest;
        c.id = std::string(line);
        c.name = last_part(line, "::");
        c.group = std::string(line.substr(0, sep));
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<TestOutcome> parse_junit(std::string_view xml) {
    std::vector<TestOutcome> out;
    for (size_t at = 0; (at = xml.find("<testcase", at)) != std::string_view::npos;) {
        const size_t close = xml.find('>', at);
        if (close == std::string_view::npos) break;
        const std::string_view tag = xml.substr(at, close - at);
        TestOutcome o;
        o.name = attribute(tag, "name");
        o.classname = attribute(tag, "classname");
        o.seconds = std::strtod(attribute(tag, "time").c_str(), nullptr);
        const bool alone = close > at && xml[close - 1] == '/';
        size_t end = close + 1;
        if (!alone) {
            const size_t done = xml.find("</testcase>", close);
            end = done == std::string_view::npos ? xml.size() : done + 11;
            const std::string_view body = xml.substr(close + 1, end - close - 1);
            if (size_t f = body.find("<failure"); f != std::string_view::npos || (f = body.find("<error")) != std::string_view::npos) {
                o.result = TestCase::kFailed;
                const size_t fc = body.find('>', f);
                o.message = attribute(body.substr(f, fc == std::string_view::npos ? std::string_view::npos : fc - f), "message");
                // (pytest's for a module it could not collect says only that:
                // why is the last of the "E   " lines its text ends with.)
                if (o.message == "collection failure")
                    for (size_t l = 0; l < body.size();) {
                        size_t nl = body.find('\n', l);
                        if (nl == std::string_view::npos) nl = body.size();
                        std::string_view line = body.substr(l, nl - l);
                        if (const size_t lt = line.find('<'); lt != std::string_view::npos) line = line.substr(0, lt);  // ("...</error>")
                        if (line.starts_with("E ") && line.find_first_not_of(' ', 1) != std::string_view::npos)
                            o.message = unescape(line.substr(line.find_first_not_of(' ', 1)));
                        l = nl + 1;
                    }
            } else if (body.find("<skipped") != std::string_view::npos) {
                o.result = TestCase::kSkipped;
            }
        }
        if (attribute(tag, "status") == "notrun") o.result = TestCase::kSkipped;
        out.push_back(std::move(o));
        at = end;
    }
    return out;
}

bool outcome_is(const TestOutcome& o, const TestCase& t) {
    if (t.runner == TestCase::kCTest) return o.name == t.id;
    // pytest: "tests/test_a.py::TestK::test_y" is classname "tests.test_a.TestK", name "test_y".
    const size_t sep = t.id.find("::");
    if (sep == std::string::npos || o.name != t.name) return false;
    std::string module = t.id.substr(0, sep);
    if (module.ends_with(".py")) module.resize(module.size() - 3);
    for (char& c : module)
        if (c == '/' || c == '\\') c = '.';
    std::string mid = t.id.substr(sep + 2);
    mid.resize(mid.size() - std::min(mid.size(), t.name.size()));
    for (size_t at = 0; (at = mid.find("::", at)) != std::string::npos;) mid.replace(at, 2, ".");
    std::string cls = module + (mid.empty() ? "" : "." + mid);
    while (!cls.empty() && cls.back() == '.') cls.pop_back();
    return o.classname == cls || o.classname.ends_with("." + cls) || cls.ends_with("." + o.classname);
}

namespace {

std::string whole(const std::string& path, const std::string& dir) {
    if (path.empty()) return path;
    return std::filesystem::path(path[0] == '/' ? path : dir + "/" + path).lexically_normal().string();  // (as a document's path is: no ".." in it)
}

void finish(FileCoverage& c) {
    std::ranges::sort(c.hit);
    std::ranges::sort(c.missed);
    const size_t all = c.hit.size() + c.missed.size();
    c.percent = all ? 100.0 * static_cast<double>(c.hit.size()) / static_cast<double>(all) : 0;
}

}  // namespace

std::vector<std::pair<std::string, FileCoverage>> parse_gcov_json(std::string_view text, const std::string& dir) {
    std::vector<std::pair<std::string, FileCoverage>> out;
    auto file_of = [&](const std::string& path) -> FileCoverage& {
        for (auto& [p, c] : out)
            if (p == path) return c;
        out.emplace_back(path, FileCoverage{});
        return out.back().second;
    };
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (line.find('{') == std::string_view::npos) continue;
        json::Doc doc;
        if (!doc.parse(std::string(line))) continue;
        const std::string cwd(doc.root()["current_working_directory"].str());
        for (const json::Value f : doc.root()["files"]) {
            const std::string path = whole(std::string(f["file"].str()), cwd.empty() ? dir : cwd);
            if (path.empty() || path.starts_with("/usr/") || path.find("/include/c++/") != std::string::npos) continue;  // (the system's headers)
            FileCoverage& c = file_of(path);
            for (const json::Value l : f["lines"]) {
                const u32 n = static_cast<u32>(l["line_number"].number());
                if (!n) continue;
                // (A line in several functions, an inline one's say: run if any ran it.)
                const bool ran = l["count"].number() > 0;
                std::erase(c.missed, n);
                if (ran) {
                    if (std::ranges::find(c.hit, n) == c.hit.end()) c.hit.push_back(n);
                } else if (std::ranges::find(c.hit, n) == c.hit.end()) {
                    c.missed.push_back(n);
                }
            }
        }
    }
    for (auto& [p, c] : out) finish(c);
    return out;
}

std::vector<std::pair<std::string, FileCoverage>> parse_coverage_py(std::string_view text, const std::string& dir) {
    std::vector<std::pair<std::string, FileCoverage>> out;
    json::Doc doc;
    if (!doc.parse(std::string(text))) return out;
    for (const json::Value f : doc.root()["files"]) {
        FileCoverage c;
        for (const json::Value n : f["executed_lines"]) c.hit.push_back(static_cast<u32>(n.number()));
        for (const json::Value n : f["missing_lines"]) c.missed.push_back(static_cast<u32>(n.number()));
        finish(c);
        out.emplace_back(whole(std::string(f.key()), dir), std::move(c));
    }
    return out;
}

std::vector<std::pair<u32, std::string>> find_test_defs(std::string_view text, bool python) {
    std::vector<std::pair<u32, std::string>> out;
    std::string cls;      // (Python: the Test class the lines are in)
    size_t cls_indent = 0;
    u32 line = 0;
    for (size_t at = 0; at < text.size(); ++line) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view l = text.substr(at, end - at);
        at = end + 1;
        size_t i = 0;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
        const std::string_view s = l.substr(i);
        auto ident = [&](std::string_view from, size_t* pos) {
            while (*pos < from.size() && from[*pos] == ' ') ++*pos;
            const size_t b = *pos;
            while (*pos < from.size() && word_char(from[*pos])) ++*pos;
            return std::string(from.substr(b, *pos - b));
        };
        if (python) {
            if (!s.empty() && i <= cls_indent && !cls.empty() && !s.starts_with("#")) cls.clear();
            if (s.starts_with("class ")) {
                size_t p = 6;
                const std::string name = ident(s, &p);
                if (name.starts_with("Test")) cls = name, cls_indent = i;
                continue;
            }
            std::string_view d = s;
            if (d.starts_with("async ")) d.remove_prefix(6);
            if (!d.starts_with("def ")) continue;
            size_t p = 4;
            const std::string name = ident(d, &p);
            if (name.starts_with("test")) out.emplace_back(line, (!cls.empty() && i > cls_indent ? cls + "::" : "") + name);
            continue;
        }
        auto macro = [&](std::string_view m) { return s.starts_with(m) && s.size() > m.size() && (s[m.size()] == '(' || s[m.size()] == ' '); };
        auto quoted_after = [&](size_t from) -> std::string {  // the first string literal from there
            const size_t q = s.find('"', from);
            if (q == std::string_view::npos) return {};
            std::string out_s;
            for (size_t k = q + 1; k < s.size() && s[k] != '"'; ++k) {
                if (s[k] == '\\' && k + 1 < s.size()) ++k;
                out_s += s[k];
            }
            return out_s;
        };
        if (macro("TEST") || macro("TEST_F") || macro("TEST_P") || macro("TYPED_TEST")) {
            size_t p = s.find('(') + 1;
            const std::string a = ident(s, &p);
            while (p < s.size() && (s[p] == ' ' || s[p] == ',')) ++p;
            const std::string b = ident(s, &p);
            if (!a.empty() && !b.empty()) out.emplace_back(line, a + "." + b);
        } else if (macro("TEST_CASE") || macro("TEST_CASE_METHOD") || macro("TEST_CASE_FIXTURE")) {
            const std::string name = quoted_after(s.find('('));
            if (!name.empty()) out.emplace_back(line, name);
        } else if (macro("SCENARIO")) {
            const std::string name = quoted_after(s.find('('));
            if (!name.empty()) out.emplace_back(line, "Scenario: " + name);
        }
    }
    return out;
}

}  // namespace blit
