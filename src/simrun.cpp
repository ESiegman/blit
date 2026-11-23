#include "simrun.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>

namespace blit {

namespace {

std::string_view base_of(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_dump(std::string_view name) {
    const std::string l = lower(name);
    return l.size() > 4 && (l.ends_with(".vcd") || l.ends_with(".fst"));
}

bool is_source(std::string_view name) {
    const std::string l = lower(name);
    return l.ends_with(".v") || l.ends_with(".sv") || l.ends_with(".vh") || l.ends_with(".svh");
}

// What a run's output says of the dump it wrote: the words before its name
// (anywhere in a line: SymbiYosys's begin with its time and task), the
// words after it (none: to the end of the line), and how good a choice it
// is (lower first).
struct Said {
    std::string_view before, after;
    int rank;
};
constexpr Said kSaid[] = {
    {"counterexample trace: ", "", 0},              // SymbiYosys, an assertion that failed
    {"VCD info: dumpfile ", " opened for output.", 1},  // Icarus's vvp
    {"FST info: dumpfile ", " opened for output.", 1},
    {"cover trace: ", "", 2},                       // SymbiYosys, a cover statement reached
};

// Verilator's program, from the folder it was built in: obj_dir (or
// another obj_ folder, or the --Mdir given) and V and the top's name.
bool verilated(std::string_view word, std::string_view mdir) {
    const size_t slash = word.find_last_of('/');
    if (slash == std::string_view::npos || slash + 2 > word.size() || word[slash + 1] != 'V') return false;
    std::string_view dir = word.substr(0, slash);
    while (dir.starts_with("./")) dir.remove_prefix(2);
    return base_of(dir).starts_with("obj") || (!mdir.empty() && (dir == mdir || base_of(dir) == base_of(mdir)));
}

}  // namespace

std::vector<std::string> sh_words(std::string_view cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool word = false;
    auto flush = [&] {
        if (word) out.push_back(std::move(cur));
        cur.clear();
        word = false;
    };
    for (size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];
        if (c == '\'') {
            word = true;
            const size_t end = cmd.find('\'', i + 1);
            cur.append(cmd.substr(i + 1, (end == std::string_view::npos ? cmd.size() : end) - i - 1));
            i = end == std::string_view::npos ? cmd.size() : end;
        } else if (c == '"') {
            word = true;
            for (++i; i < cmd.size() && cmd[i] != '"'; ++i) {
                if (cmd[i] == '\\' && i + 1 < cmd.size() && std::string_view("\"\\$`").find(cmd[i + 1]) != std::string_view::npos) ++i;
                cur += cmd[i];
            }
        } else if (c == '\\' && i + 1 < cmd.size()) {
            word = true;
            cur += cmd[++i];
        } else if (c == ' ' || c == '\t' || c == '\n') {
            flush();
        } else if (std::string_view(";&|()<>").find(c) != std::string_view::npos) {
            flush();
            size_t j = i;
            while (j < cmd.size() && std::string_view(";&|()<>").find(cmd[j]) != std::string_view::npos && j - i < 2) ++j;
            out.emplace_back(cmd.substr(i, j - i));
            i = j - 1;
        } else {
            word = true;
            cur += c;
        }
    }
    flush();
    return out;
}

std::string_view sim_tool(std::string_view cmd) {
    const std::vector<std::string> words = sh_words(cmd);
    std::string_view mdir;
    bool binary = false;
    for (size_t i = 0; i < words.size(); ++i) {
        if (words[i] == "--Mdir" && i + 1 < words.size()) mdir = words[i + 1];
        binary |= words[i] == "--binary";
    }
    for (const std::string& w : words) {
        const std::string_view b = base_of(w);
        if (b == "iverilog" || b == "vvp") return "Icarus";
        if (b == "sby") return "SymbiYosys";
        if ((b == "verilator" && binary) || verilated(w, mdir)) return "Verilator";
    }
    return {};
}

std::vector<std::string> sim_dumps_said(std::string_view output) {
    std::vector<std::pair<int, std::string>> found;  // (rank, name), in the order said
    for (size_t at = 0; at < output.size();) {
        const size_t nl = std::min(output.find('\n', at), output.size());
        std::string_view line = output.substr(at, nl - at);
        at = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        for (const Said& s : kSaid) {
            const size_t p = line.find(s.before);
            if (p == std::string_view::npos) continue;
            std::string_view name = line.substr(p + s.before.size());
            if (!s.after.empty()) {
                const size_t q = name.rfind(s.after);
                if (q == std::string_view::npos) continue;
                name = name.substr(0, q);
            }
            if (is_dump(name)) found.emplace_back(s.rank, std::string(name));
        }
    }
    // Best first; of one kind, the last said first (the last run of several).
    std::vector<std::string> out;
    for (int rank = 0; rank <= 2; ++rank)
        for (auto it = found.rbegin(); it != found.rend(); ++it)
            if (it->first == rank) out.push_back(it->second);
    return out;
}

void sim_named(std::string_view cmd, std::vector<std::string>* dumps, std::vector<std::string>* sources) {
    for (std::string w : sh_words(cmd)) {
        // (After an = or a +: +dumpfile=x.vcd, '-DDUMP="x.vcd"', its quotes taken off.)
        if (const size_t eq = w.find('='); eq != std::string::npos) w = w.substr(eq + 1);
        while (!w.empty() && (w.front() == '+' || w.front() == '"' || w.front() == '\'')) w.erase(w.begin());
        while (!w.empty() && (w.back() == '"' || w.back() == '\'')) w.pop_back();
        if (is_dump(w)) dumps->push_back(w);
        else if (is_source(w) && !w.starts_with("-")) sources->push_back(w);
    }
}

std::vector<std::string> sim_dumpfiles(std::string_view text) {
    std::vector<std::string> out;
    for (size_t at = text.find("$dumpfile"); at != std::string_view::npos; at = text.find("$dumpfile", at + 1)) {
        size_t i = at + 9;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size() || text[i] != '(') continue;
        for (++i; i < text.size() && (text[i] == ' ' || text[i] == '\t'); ++i) {}
        if (i >= text.size() || text[i] != '"') continue;
        const size_t end = text.find('"', i + 1);
        if (end == std::string_view::npos || end - i - 1 > 4096) continue;
        const std::string name(text.substr(i + 1, end - i - 1));
        if (is_dump(name)) out.push_back(name);
    }
    return out;
}

std::string sim_choose(const std::vector<SimDump>& found, const std::vector<std::string>& said, const std::vector<std::string>& named) {
    auto match = [](const std::string& path, std::string_view name) {
        while (name.starts_with("./")) name.remove_prefix(2);
        return path == name || (path.size() > name.size() && path.ends_with(name) && path[path.size() - name.size() - 1] == '/') ||
               (name.starts_with('/') && name.ends_with("/" + path));
    };
    for (const std::string& s : said)
        for (const SimDump& f : found)
            if (match(f.path, s)) return f.path;
    for (const SimDump& f : found)
        for (const std::string& n : named)
            if (match(f.path, n)) return f.path;
    return found.empty() ? std::string() : found.front().path;
}

std::vector<SimDump> sim_find_here(const std::string& dir, i64 since) {
    namespace fs = std::filesystem;
    std::vector<SimDump> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (name.starts_with('.') || name == "node_modules" || it.depth() >= 5) it.disable_recursion_pending();
            continue;
        }
        if (!is_dump(name) || name.starts_with('.')) continue;
        struct stat st{};
        if (::stat(it->path().c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_mtime < since) continue;
        out.push_back({it->path().lexically_relative(dir).string(), static_cast<i64>(st.st_mtime)});
    }
    std::ranges::stable_sort(out, [](const SimDump& a, const SimDump& b) { return a.when > b.when || (a.when == b.when && a.path < b.path); });
    return out;
}

std::string sim_find_command(i64 ago) {
    // (Minutes, as both finds take them, for the run's and a minute more; the seconds are sorted out from the times.)
    const std::string recent = ago >= 0 ? " -mmin -" + std::to_string(ago / 60 + 2) : std::string();
    return "date +%s; find . -maxdepth 6 \\( -path '*/.*' -o -name node_modules \\) -prune -o -type f \\( -name '*.vcd' -o -name '*.fst' -o -name '*.VCD' -o -name '*.FST' \\)" +
           recent + " -exec sh -c 'stat -c \"%Y %n\" \"$@\" 2>/dev/null || stat -f \"%m %N\" \"$@\"' sh {} + 2>/dev/null";
}

std::vector<SimDump> sim_find_read(std::string_view out, i64 ago) {
    std::vector<SimDump> found;
    i64 now = -1;
    for (size_t at = 0; at < out.size();) {
        const size_t nl = std::min(out.find('\n', at), out.size());
        std::string_view line = out.substr(at, nl - at);
        at = nl + 1;
        i64 when = 0;
        const auto [p, ec] = std::from_chars(line.data(), line.data() + line.size(), when);
        if (ec != std::errc()) continue;
        if (now < 0) {
            now = when;
            continue;
        }
        if (p == line.data() + line.size() || *p != ' ') continue;
        std::string path(line.substr(static_cast<size_t>(p - line.data()) + 1));
        if (path.starts_with("./")) path.erase(0, 2);
        if (ago >= 0 && when < now - ago - 2) continue;
        found.push_back({std::move(path), when});
    }
    std::ranges::stable_sort(found, [](const SimDump& a, const SimDump& b) { return a.when > b.when || (a.when == b.when && a.path < b.path); });
    return found;
}

}  // namespace blit
