#include "notebook.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <random>

#include "json.hpp"

namespace blit {

bool is_notebook_path(std::string_view path) { return path.ends_with(".ipynb"); }

namespace {

// A cell's text: its "source", a string or (as Jupyter writes it) its lines.
std::string source_of(json::Value cell) {
    const json::Value s = cell["source"];
    if (s.is_string()) return std::string(s.str());
    std::string out;
    for (const json::Value line : s) out += line.str();
    return out;
}

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

// JSON as Jupyter writes it (Python's json.dumps with indent=1 and its keys
// sorted), so that a notebook written back differs only where it changed.
void pretty(std::string& out, json::Value v, int depth) {
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
                pretty(out, e, depth + 1);
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
            for (const json::Value& m : members) {
                if (!first) out += ',';
                first = false;
                line(depth + 1);
                json::quote(out, m.key());
                out += ": ";
                pretty(out, m, depth + 1);
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
        pretty(s, doc.root(), 3);  // (as the file has them)
        cell->outputs = std::make_shared<const std::string>(std::move(s));
    }
    return true;
}

std::vector<CellMatch> notebook_match(const std::vector<NotebookCell>& now, const std::vector<NotebookCell>& model) {
    std::vector<CellMatch> out(now.size());
    std::vector<bool> taken(model.size(), false);
    for (size_t i = 0; i < now.size(); ++i)
        for (size_t j = 0; j < model.size(); ++j)
            if (!taken[j] && model[j].kind == now[i].kind && model[j].source == now[i].source) {
                out[i] = {static_cast<int>(j), true};
                taken[j] = true;
                break;
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
        // its text, and (if its text is the one they are of) its outputs
        // and count.
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
            const NotebookCell* r = ran[i].same ? &(*model)[static_cast<size_t>(ran[i].at)] : nullptr;
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
