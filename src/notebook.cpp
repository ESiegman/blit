#include "notebook.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <random>
#include <unordered_map>

#include "json.hpp"

namespace blit {

bool is_notebook_path(std::string_view path) { return path.ends_with(".ipynb"); }

namespace {

// A text as Jupyter keeps one: a string, or its lines.
std::string source_of_text(json::Value s) {
    if (s.is_string()) return std::string(s.str());
    std::string out;
    for (const json::Value line : s) out += line.str();
    return out;
}

// A cell's text: its "source".
std::string source_of(json::Value cell) { return source_of_text(cell["source"]); }

std::string_view chomp(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// The last line of a text with something in it.
std::string last_line(std::string_view s) {
    s = chomp(s);
    const size_t nl = s.find_last_of('\n');
    std::string line(nl == std::string_view::npos ? s : s.substr(nl + 1));
    std::erase(line, '\r');
    return line;
}

// A code cell's outputs in a line: what it printed last, or what it gave
// (without the escapes that colour a terminal's text).
std::string output_of(json::Value outputs) {
    std::string out;
    for (const json::Value o : outputs) {
        const std::string_view type = o["output_type"].str();
        if (type == "stream") {
            const json::Value t = o["text"];
            std::string text;
            if (t.is_string()) text = t.str();
            else
                for (const json::Value l : t) text += l.str();
            if (!chomp(text).empty()) out = last_line(text);
        } else if (type == "error") {
            out = std::string(o["ename"].str()) + ": " + std::string(o["evalue"].str());
        } else if (type == "execute_result" || type == "display_data") {
            const json::Value data = o["data"];
            std::string pic;
            for (const json::Value m : data)
                if (m.key().starts_with("image/")) pic = "[" + std::string(m.key()) + "]";
            if (!pic.empty()) {
                out = pic;
            } else if (const json::Value t = data["text/plain"]) {
                std::string text;
                if (t.is_string()) text = t.str();
                else
                    for (const json::Value l : t) text += l.str();
                out = last_line(text);
            }
        }
    }
    for (size_t esc; (esc = out.find('\x1b')) != std::string::npos;) {
        size_t end = esc + 1;  // (its last byte)
        if (end < out.size() && out[end] == '[') {  // a colour (CSI): to its final byte
            for (++end; end < out.size() && !(out[end] >= '@' && out[end] <= '~');) ++end;
        } else if (end < out.size() && out[end] == ']') {  // a title or a link (OSC): to BEL, or ESC backslash
            for (++end; end < out.size() && out[end] != '\a' && !(out[end] == '\\' && out[end - 1] == '\x1b');) ++end;
        }
        out.erase(esc, std::min(end + 1, out.size()) - esc);
    }
    for (char& c : out)
        if (c == '\t') c = ' ';
    return out;
}

// A text's lines, each with what ends it, as Python's splitlines(True)
// makes them (a newline, a carriage return or both, and the rarer breaks
// it knows: \v, \f, \x1c-\x1e, NEL, U+2028, U+2029).
std::vector<std::string_view> split_lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c == '\r') len = i + 1 < s.size() && s[i + 1] == '\n' ? 2 : 1;
        else if (c == '\n' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1e)) len = 1;
        else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x85) len = 2;
        else if (c == 0xE2 && i + 2 < s.size() && s[i + 1] == '\x80' && (s[i + 2] == '\xA8' || s[i + 2] == '\xA9')) len = 3;
        if (!len) {
            ++i;
            continue;
        }
        out.push_back(s.substr(start, i + len - start));
        start = i += len;
    }
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}

// What of an output Jupyter writes as its lines (nbformat's split_lines):
// a stream's text, and the text/*, SVG and JavaScript in its data.
enum Split : u8 { kWhole, kOutputs, kOutput, kData };

// JSON as Jupyter writes it (Python's json.dumps with indent=1 and its keys
// sorted), so that a notebook written back differs only where it changed;
// `split`: the list of a cell's outputs, or part of one.
void pretty(std::string& out, json::Value v, int depth, Split split = kWhole) {
    auto line = [&](int d) {
        out += '\n';
        out.append(static_cast<size_t>(d), ' ');
    };
    switch (v.type()) {
        case json::Type::Number: {
            const double n = v.number();
            char buf[32];
            if (n == std::floor(n) && std::fabs(n) < 9e15) {
                const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<i64>(n));
                out.append(buf, static_cast<size_t>(r.ptr - buf));
            } else if (std::isfinite(n)) {
                const auto r = std::to_chars(buf, buf + sizeof buf, n);
                out.append(buf, static_cast<size_t>(r.ptr - buf));
            } else {
                out += "NaN";
            }
            break;
        }
        case json::Type::Array: {
            if (v.size() == 0) {
                out += "[]";
                break;
            }
            out += '[';
            bool first = true;
            for (const json::Value e : v) {
                if (!first) out += ',';
                first = false;
                line(depth + 1);
                pretty(out, e, depth + 1, split == kOutputs ? kOutput : kWhole);
            }
            line(depth);
            out += ']';
            break;
        }
        case json::Type::Object: {
            if (v.size() == 0) {
                out += "{}";
                break;
            }
            std::vector<json::Value> members;
            for (const json::Value m : v) members.push_back(m);
            std::stable_sort(members.begin(), members.end(), [](const json::Value& a, const json::Value& b) { return a.key() < b.key(); });
            out += '{';
            bool first = true;
            const bool stream = split == kOutput && v["output_type"].str() == "stream";
            for (const json::Value& m : members) {
                if (!first) out += ',';
                first = false;
                line(depth + 1);
                json::quote(out, m.key());
                out += ": ";
                const std::string_view k = m.key();
                const bool lines = m.is_string() && ((stream && k == "text") || (split == kData && (k.starts_with("text/") || k == "image/svg+xml" || k == "application/javascript")));
                if (!lines) {
                    pretty(out, m, depth + 1, split == kOutput && k == "data" ? kData : kWhole);
                    continue;
                }
                const std::vector<std::string_view> each = split_lines(m.str());
                if (each.empty()) {
                    out += "[]";
                    continue;
                }
                out += '[';
                for (size_t n = 0; n < each.size(); ++n) {
                    if (n) out += ',';
                    line(depth + 2);
                    json::quote(out, each[n]);
                }
                line(depth + 1);
                out += ']';
            }
            line(depth);
            out += '}';
            break;
        }
        default: json::dump(out, v); break;
    }
}

// A cell's text as Jupyter's "source": its lines, each but the last with its newline.
void source_lines(std::string& out, std::string_view text, int depth) {
    if (text.empty()) {
        out += "[]";
        return;
    }
    out += '[';
    size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        const size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
        if (!first) out += ',';
        first = false;
        out += '\n';
        out.append(static_cast<size_t>(depth + 1), ' ');
        json::quote(out, text.substr(at, end - at));
        at = end;
    }
    out += '\n';
    out.append(static_cast<size_t>(depth), ' ');
    out += ']';
}

bool head_line(std::string_view line) { return line.starts_with("# %%") || line == "#%%" || line.starts_with("#%% "); }

std::string kind_of_head(std::string_view line) {
    if (line.find("[markdown]") != std::string_view::npos || line.find("[md]") != std::string_view::npos) return "markdown";
    if (line.find("[raw]") != std::string_view::npos) return "raw";
    return "code";
}

}  // namespace

bool notebook_read(std::string_view text, std::vector<NotebookCell>* cells, std::string* script) {
    json::Doc doc;
    if (!doc.parse(std::string(text)) || !doc.root().is_object() || !doc.root()["cells"].is_array()) return false;
    cells->clear();
    script->clear();
    for (const json::Value c : doc.root()["cells"]) {
        NotebookCell cell;
        cell.kind = std::string(c["cell_type"].str());
        if (cell.kind != "markdown" && cell.kind != "raw") cell.kind = "code";
        cell.source = std::string(chomp(source_of(c)));
        if (cell.kind == "code") {
            const json::Value outputs = c["outputs"];
            cell.output = output_of(outputs);
            if (outputs.is_array() && outputs.size()) {
                std::string s;
                pretty(s, outputs, 3);
                cell.outputs = std::make_shared<const std::string>(std::move(s));
            }
            if (c["execution_count"].is_number()) cell.count = c["execution_count"].integer();
        }
        if (!script->empty()) *script += "\n";
        *script += cell.kind == "code" ? "# %%\n" : cell.kind == "markdown" ? "# %% [markdown]\n" : "# %% [raw]\n";
        if (cell.kind == "markdown") {  // (each line commented: the script is still Python)
            size_t at = 0;
            while (at <= cell.source.size() && !cell.source.empty()) {
                size_t nl = cell.source.find('\n', at);
                if (nl == std::string::npos) nl = cell.source.size();
                const std::string_view l(cell.source.data() + at, nl - at);
                *script += l.empty() ? "#" : "# ";
                *script += l;
                *script += '\n';
                at = nl + 1;
            }
        } else if (!cell.source.empty()) {
            *script += cell.source;
            *script += '\n';
        }
        cells->push_back(std::move(cell));
    }
    return true;
}

std::string notebook_language(std::string_view text) {
    json::Doc doc;
    if (!doc.parse(std::string(text))) return "python";
    const json::Value meta = doc.root()["metadata"];
    std::string lang(meta["language_info"]["name"].str());
    if (lang.empty()) lang = meta["kernelspec"]["language"].str();
    for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lang.empty() ? "python" : lang;
}

bool notebook_set_outputs(NotebookCell* cell, std::string_view outputs, i64 count) {
    json::Doc doc;
    if (!doc.parse(std::string(outputs)) || !doc.root().is_array()) return false;
    cell->output = output_of(doc.root());
    cell->count = count;
    cell->outputs = nullptr;
    if (doc.root().size()) {
        std::string s;
        pretty(s, doc.root(), 3, kOutputs);  // (as Jupyter writes them in the file)
        cell->outputs = std::make_shared<const std::string>(std::move(s));
    }
    return true;
}

namespace {

// Text as a terminal leaves each of its lines: what is after its last
// carriage return (one at its end is kept, for what comes next to write over).
std::string overwritten(std::string_view text) {
    std::string kept;
    kept.reserve(text.size());
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find('\n', at);
        const bool ended = nl != std::string_view::npos;
        if (!ended) nl = text.size();
        std::string_view line = text.substr(at, nl - at);
        const bool back = line.ends_with('\r');
        while (line.ends_with('\r')) line.remove_suffix(1);
        if (const size_t cr = line.rfind('\r'); cr != std::string_view::npos) line.remove_prefix(cr + 1);
        kept += line;
        if (ended) kept += '\n';
        else if (back) kept += '\r';
        at = nl + 1;
    }
    return kept;
}

// Where a UTF-8 character starts, at or before `at`.
size_t char_start(std::string_view s, size_t at) {
    while (at > 0 && at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
    return at;
}

}  // namespace

bool NotebookOutputs::add(std::string_view output) {
    json::Doc doc;
    if (!doc.parse(std::string(output)) || !doc.root().is_object()) return false;
    const json::Value o = doc.root();
    if (o["output_type"].str() != "stream") {
        outs_.emplace_back();
        outs_.back().json = output;
        return true;
    }
    const std::string_view name = o["name"].str();
    const std::string piece = source_of_text(o["text"]);
    if (outs_.empty() || !outs_.back().json.empty() || outs_.back().stream != name) {  // (after the same stream's text: one text)
        outs_.emplace_back();
        outs_.back().stream = name;
    }
    Out& s = outs_.back();
    // What the piece can write over: its text's last line, if it goes back
    // over it (a carriage return before its first newline, or one that line
    // ends with); else nothing before it.
    const size_t cr = piece.find('\r'), nl = piece.find('\n');
    const bool back = (cr != std::string::npos && (nl == std::string::npos || cr < nl)) || s.text.ends_with('\r');
    const size_t from = back ? s.line : s.text.size();
    const std::string kept = overwritten(back ? s.text.substr(from) + piece : piece);
    s.text.resize(from);
    for (size_t k = kept.find('\n'); k != std::string::npos; k = kept.find('\n', k + 1)) ++s.lines, s.line = from + k + 1;
    s.text += kept;
    return true;
}

std::string NotebookOutputs::list(size_t most) const {
    std::string out = "[";
    for (const Out& o : outs_) {
        if (out.size() > 1) out += ", ";
        if (o.stream.empty()) {
            out += o.json;
            continue;
        }
        out += "{\"name\": ";
        json::quote(out, o.stream);
        out += ", \"output_type\": \"stream\", \"text\": ";
        if (o.text.size() <= most) {
            json::quote(out, o.text);
            out += "}";
            continue;
        }
        // Its first lines (as many as a view shows, and some), and its last.
        const std::string_view text = o.text, body = chomp(text);
        size_t head = 0;
        for (int k = 0; k < 64 && head < text.size(); ++k) {
            const size_t nl = text.find('\n', head);
            head = nl == std::string_view::npos ? text.size() : nl + 1;
        }
        if (head > most / 2) {  // (too long: as many whole lines as fit, else as much of the first)
            const size_t nl = text.rfind('\n', most / 2 - 1);
            head = nl != std::string_view::npos ? nl + 1 : char_start(text, most / 2);
        }
        const size_t last = body.find_last_of('\n') == std::string_view::npos ? 0 : body.find_last_of('\n') + 1;
        std::string cut(text.substr(0, head));
        if (!cut.ends_with('\n')) cut += '\n';
        if (last >= head) cut += text.substr(char_start(text, std::max(last, text.size() - most / 2)));
        json::quote(out, cut);
        const u64 after = static_cast<u64>(std::count(text.begin() + static_cast<std::ptrdiff_t>(body.size()), text.end(), '\n'));  // (the newlines it ends with)
        out += ", \"blit_lines\": " + std::to_string(o.lines - after) + "}";
    }
    out += "]";
    return out;
}

std::vector<CellMatch> notebook_match(const std::vector<NotebookCell>& now, const std::vector<NotebookCell>& model) {
    std::vector<CellMatch> out(now.size());
    std::vector<bool> taken(model.size(), false);
    // The model's cells by their kind and text, each such list in order
    // (the first not taken is at `next`): found at once however many
    // cells there are, as notebook_match runs on each change.
    struct Same {
        std::vector<size_t> at;
        size_t next = 0;
    };
    std::map<std::string_view, std::unordered_map<std::string_view, Same>> by;
    for (size_t j = 0; j < model.size(); ++j) by[model[j].kind][model[j].source].at.push_back(j);
    for (size_t i = 0; i < now.size(); ++i) {
        const auto kind = by.find(now[i].kind);
        if (kind == by.end()) continue;
        const auto it = kind->second.find(now[i].source);
        if (it == kind->second.end() || it->second.next == it->second.at.size()) continue;
        const size_t j = it->second.at[it->second.next++];
        out[i] = {static_cast<int>(j), true};
        taken[j] = true;
    }
    for (size_t i = 0; i < now.size(); ++i)
        if (out[i].at < 0 && i < model.size() && !taken[i] && model[i].kind == now[i].kind) out[i].at = static_cast<int>(i), taken[i] = true;
    return out;
}

std::vector<CellLines> notebook_cell_lines(std::string_view script) {
    std::vector<std::string_view> lines;
    for (size_t at = 0; at < script.size();) {
        size_t nl = script.find('\n', at);
        if (nl == std::string_view::npos) nl = script.size();
        lines.push_back(script.substr(at, nl - at));
        at = nl + 1;
    }
    std::vector<CellLines> out;
    for (u64 n = 0; n < lines.size(); ++n) {
        if (head_line(lines[n])) {
            if (!out.empty()) out.back().end = n;
            out.push_back({n, n + 1, n + 1, kind_of_head(lines[n]) == "code"});
        } else if (out.empty() && !lines[n].empty()) {
            out.push_back({0, 0, 0, true});  // (text before the first: a code cell, without a "# %%")
        }
    }
    if (!out.empty()) out.back().end = lines.size();
    // (A cell's last lines, if blank, are the gap before the next: not its.)
    for (CellLines& c : out)
        while (c.end > c.first && lines[c.end - 1].find_first_not_of(" \t\r") == std::string_view::npos) --c.end;
    return out;
}

std::vector<NotebookCell> notebook_cells(std::string_view script) {
    std::vector<NotebookCell> out;
    std::vector<std::string_view> lines;
    for (size_t at = 0; at < script.size();) {
        size_t nl = script.find('\n', at);
        if (nl == std::string_view::npos) nl = script.size();
        lines.push_back(script.substr(at, nl - at));
        at = nl + 1;
    }
    for (const CellLines& c : notebook_cell_lines(script)) {
        NotebookCell cell;
        cell.kind = c.head == c.first ? "code" : kind_of_head(lines[c.head]);
        for (u64 l = c.first; l < c.end; ++l) {
            std::string_view line = lines[l];
            if (cell.kind == "markdown") {  // (its comment marks off)
                if (line.starts_with("# ")) line.remove_prefix(2);
                else if (line.starts_with("#")) line.remove_prefix(1);
            }
            if (l > c.first) cell.source += '\n';
            cell.source += line;
        }
        out.push_back(std::move(cell));
    }
    return out;
}

std::string notebook_write(std::string_view original, std::string_view script, const std::vector<NotebookCell>* model) {
    json::Doc doc;
    const bool had = doc.parse(std::string(original)) && doc.root().is_object();
    const json::Value root = had ? doc.root() : json::Value();
    // The notebook's cells, as they were; each of the script's matched to
    // one of them (its id and metadata), and to one of the model's (its
    // outputs and count).
    std::vector<json::Value> old;
    std::vector<NotebookCell> was;
    for (const json::Value c : root["cells"]) {
        old.push_back(c);
        NotebookCell cell;
        cell.kind = c["cell_type"].str() == "markdown" || c["cell_type"].str() == "raw" ? std::string(c["cell_type"].str()) : std::string("code");
        cell.source = std::string(chomp(source_of(c)));
        was.push_back(std::move(cell));
    }
    std::vector<NotebookCell> own;
    if (!model) {
        std::string ignored;
        if (had) notebook_read(original, &own, &ignored);
        model = &own;
    }
    const std::vector<NotebookCell> cells = notebook_cells(script);
    const std::vector<CellMatch> match = notebook_match(cells, was), ran = notebook_match(cells, *model);
    std::mt19937_64 rng(std::random_device{}());
    std::string out = "{\n \"cells\": [";
    for (size_t i = 0; i < cells.size(); ++i) {
        const NotebookCell& c = cells[i];
        const json::Value o = match[i].at >= 0 ? old[static_cast<size_t>(match[i].at)] : json::Value();
        out += i ? ",\n  {" : "\n  {";
        // Its members in order of name: what the notebook had, its kind,
        // its text, and its outputs and count.
        std::map<std::string, std::string> m;
        for (const json::Value v : o) {
            const std::string_view k = v.key();
            if (k == "cell_type" || k == "source" || k == "outputs" || k == "execution_count") continue;
            std::string s;
            pretty(s, v, 3);
            m[std::string(k)] = std::move(s);
        }
        m["cell_type"] = std::string();
        json::quote(m["cell_type"], c.kind);
        m["source"] = std::string();
        if (match[i].same) pretty(m["source"], o["source"], 3);  // (as it was written: a string, or its lines)
        else source_lines(m["source"], c.source, 3);
        if (!m.contains("metadata")) m["metadata"] = "{}";
        if (c.kind == "code") {
            const NotebookCell* r = ran[i].at >= 0 ? &(*model)[static_cast<size_t>(ran[i].at)] : nullptr;  // (changed since: kept, as Jupyter keeps them)
            m["outputs"] = r && r->outputs ? *r->outputs : "[]";
            m["execution_count"] = r && r->count >= 0 ? std::to_string(r->count) : "null";
        }
        const bool ids = !root["nbformat"].exists() || root["nbformat"].integer() > 4 || (root["nbformat"].integer() == 4 && root["nbformat_minor"].integer() >= 5);
        if (ids && !m.contains("id")) {
            char id[17];
            std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(rng()));
            m["id"] = std::string("\"") + std::string(id, 8) + "\"";
        }
        bool first = true;
        for (const auto& [k, v] : m) {
            out += first ? "\n   " : ",\n   ";
            first = false;
            json::quote(out, k);
            out += ": ";
            out += v;
        }
        out += "\n  }";
    }
    out += cells.empty() ? "]" : "\n ]";
    // The rest of the notebook as it was (or a new one's).
    std::map<std::string, std::string> top;
    for (const json::Value v : root) {
        if (v.key() == "cells") continue;
        std::string s;
        pretty(s, v, 1);
        top[std::string(v.key())] = std::move(s);
    }
    if (!top.contains("metadata")) top["metadata"] = "{}";
    if (!top.contains("nbformat")) top["nbformat"] = "4";
    if (!top.contains("nbformat_minor")) top["nbformat_minor"] = "5";
    for (const auto& [k, v] : top) {
        out += ",\n ";
        json::quote(out, k);
        out += ": ";
        out += v;
    }
    out += "\n}\n";
    return out;
}

}  // namespace blit
