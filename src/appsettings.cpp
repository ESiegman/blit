// The settings screen: a tab of every setting, under the headings
// init.example.lua groups them by, each with its value and what the
// template says of it. Enter changes one (a switch is flipped, a theme
// picked, anything else typed); Delete puts it back to its default; O opens
// init.lua at its line. A change is written into init.lua as a line
// (`blit.opt.KEY = VALUE`), which is what counts: the line that sets it
// changed, else the template's commented-out one taken in, else one at the
// end; it is saved and run again at once (unless init.lua is open with
// changes not saved: then the line is put there, for those to be saved
// with it).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "app.hpp"
#include "applua.hpp"
#include "fsio.hpp"

namespace blit {

struct SettingsView {
    struct Item {
        std::string key;    // empty: a heading
        std::string title;  // a heading's
        std::string about;  // what the template says of it
    };
    std::vector<Item> items;     // in the order shown
    std::vector<u32> line_item;  // each line's item (~0u: a blank line)
};

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Where a line's comment starts (outside quotes), else npos.
size_t comment_at(std::string_view s) {
    char quote = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (quote) {
            if (s[i] == '\\') ++i;
            else if (s[i] == quote) quote = 0;
        } else if (s[i] == '"' || s[i] == '\'') {
            quote = s[i];
        } else if (s[i] == '-' && s[i + 1] == '-') {
            return i;
        }
    }
    return std::string_view::npos;
}

// "blit.opt.KEY = ..." (after any "--"): KEY, and where the value starts.
bool setting_line(std::string_view t, std::string* key, size_t* value) {
    std::string_view s = t.substr(std::min(t.size(), t.find_first_not_of(" \t")));
    if (s.starts_with("--")) {
        s.remove_prefix(2);
        s = s.substr(std::min(s.size(), s.find_first_not_of(' ')));
    }
    if (!s.starts_with("blit.opt.")) return false;
    s.remove_prefix(9);
    size_t n = 0;
    while (n < s.size() && (std::isalnum(static_cast<unsigned char>(s[n])) || s[n] == '_' || s[n] == '.')) ++n;
    if (!n) return false;
    std::string_view rest = s.substr(n);
    const size_t eq = rest.find_first_not_of(' ');
    if (eq == std::string_view::npos || rest[eq] != '=' || (eq + 1 < rest.size() && rest[eq + 1] == '=')) return false;
    *key = std::string(s.substr(0, n));
    *value = static_cast<size_t>(rest.data() + eq + 1 - t.data());
    return true;
}

// The length of the long bracket that opens at i ("[[", "[==["), else 0.
size_t long_open(std::string_view t, size_t i) {
    if (i >= t.size() || t[i] != '[') return 0;
    size_t j = i + 1;
    while (j < t.size() && t[j] == '=') ++j;
    return j < t.size() && t[j] == '[' ? j + 1 - i : 0;
}

// Past the Lua string, long string or comment that starts at i (npos: it
// never ends), else just past i. (A line comment ends at its newline.)
size_t lua_past(std::string_view t, size_t i) {
    const char c = t[i];
    if (c == '"' || c == '\'') {
        for (size_t j = i + 1; j < t.size(); ++j) {
            if (t[j] == '\\') ++j;
            else if (t[j] == c) return j + 1;
            else if (t[j] == '\n') return j;  // (not closed: it ends with its line)
        }
        return t.size();
    }
    size_t at = i;
    if (c == '-' && i + 1 < t.size() && t[i + 1] == '-') {
        at = i + 2;
        if (!long_open(t, at)) return std::min(t.find('\n', at), t.size());
    }
    const size_t open = long_open(t, at);
    if (!open) return i + 1;
    const std::string close = "]" + std::string(open - 2, '=') + "]";
    const size_t end = t.find(close, at + open);
    return end == std::string_view::npos ? std::string_view::npos : end + close.size();
}

// The start of the line after the one at `at`, past any long string or
// comment that carries on from it (the lines in one set nothing).
size_t next_line(std::string_view t, size_t at) {
    for (size_t i = at; i < t.size();) {
        if (t[i] == '\n') return i + 1;
        i = lua_past(t, i);
    }
    return t.size();
}

// Where a value that starts at i ends: at its line's end, a ";" or a
// comment, outside brackets (a table can take lines). npos if it is not
// closed.
size_t value_end(std::string_view t, size_t i) {
    int depth = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (depth == 0 && (c == '\n' || c == ';' || (c == '-' && i + 1 < t.size() && t[i + 1] == '-'))) return i;
        if (c == '{' || c == '(' || (c == '[' && !long_open(t, i))) ++depth;
        else if ((c == '}' || c == ')' || c == ']') && --depth < 0) return std::string_view::npos;
        if ((i = lua_past(t, i)) == std::string_view::npos) return i;
    }
    return depth == 0 ? t.size() : std::string_view::npos;
}

bool is_key(std::string_view k) {
    for (const char* c : config_keys())
        if (k == c) return true;
    return false;
}

// The first sentence of a paragraph.
std::string first_sentence(const std::string& p) {
    const size_t dot = p.find(". ");
    return dot == std::string::npos ? p : p.substr(0, dot + 1);
}

// What the template says: its headings, and under each its settings with
// what it says of them (the comment on the line, else the paragraph above).
std::vector<SettingsView::Item> items_of_template() {
    std::vector<SettingsView::Item> out;
    std::vector<std::string> seen;
    std::string para;
    const std::string_view text = init_template();
    SettingsView::Item* last = nullptr;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        const std::string_view t = trim(line);
        if (t.empty()) {
            para.clear();
            last = nullptr;
            continue;
        }
        if (t.starts_with("------")) {  // a heading
            const std::string_view name = trim(t.substr(t.find_first_not_of('-') == std::string_view::npos ? t.size() : t.find_first_not_of('-')));
            if (!name.empty()) out.push_back({{}, std::string(name), {}});
            para.clear();
            last = nullptr;
            continue;
        }
        std::string key;
        size_t value = 0;
        if (setting_line(line, &key, &value)) {
            last = nullptr;
            if (!is_key(key) || std::ranges::find(seen, key) != seen.end()) continue;
            seen.push_back(key);
            const std::string_view rest = line.substr(value);
            const size_t c = comment_at(rest);
            std::string about = c == std::string_view::npos ? first_sentence(para) : std::string(trim(rest.substr(c + 2)));
            out.push_back({key, {}, std::move(about)});
            last = &out.back();
            continue;
        }
        if (!t.starts_with("--")) {
            last = nullptr;
            continue;
        }
        std::string_view words = trim(t.substr(2));
        if (last && words.starts_with("--")) {  // (more of the comment on the setting above)
            last->about += " " + std::string(trim(words.substr(2)));
            continue;
        }
        last = nullptr;
        if (!words.empty()) para += (para.empty() ? "" : " ") + std::string(words);
    }
    // (A heading with no settings under it, as the template has for what is not a setting: left out.)
    for (size_t i = out.size(); i-- > 0;)
        if (out[i].key.empty() && (i + 1 == out.size() || out[i + 1].key.empty())) out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
    // Whatever the template leaves out, at the end.
    bool others = false;
    for (const char* k : config_keys()) {
        if (std::ranges::find(seen, std::string(k)) != seen.end()) continue;
        if (!others) out.push_back({{}, "others", {}}), others = true;
        out.push_back({k, {}, {}});
    }
    return out;
}

// A value as a setting's literal (as :set and the settings file take it).
std::string literal(const ConfigValue& v) {
    switch (v.kind) {
        case ConfigValue::kBool: return v.flag ? "true" : "false";
        case ConfigValue::kNumber: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.17g", v.number);
            return buf;
        }
        case ConfigValue::kText: return toml_quote(v.text);
        case ConfigValue::kList: {
            std::string out = "[";
            for (size_t i = 0; i < v.list.size(); ++i) out += (i ? ", " : "") + toml_quote(v.list[i]);
            return out + "]";
        }
        default: return {};
    }
}

// As it is typed in the box: the text itself, a list's items with commas.
std::string typed_of(const ConfigValue& v) {
    switch (v.kind) {
        case ConfigValue::kNumber: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%g", v.number);
            return buf;
        }
        case ConfigValue::kText: return v.text;
        case ConfigValue::kList: {
            std::string out;
            for (const std::string& s : v.list) out += (out.empty() ? "" : ", ") + s;
            return out;
        }
        default: return {};
    }
}

}  // namespace

// init.lua's text with a setting's line: `line` in place of the last line
// that sets it (its comment kept), else of the template's commented-out
// one, else at the end. With no line: each line that sets it commented
// out, so that it is the default again. A value can take lines (a table),
// and what follows it on its line (a comment, another statement) is kept;
// lines inside a long comment or string (--[[ ]]) set nothing.
std::string App::init_with(std::string text, const std::string& key, const std::string& line) {
    struct Found {
        size_t start, stop;  // its first line's start, and where what follows its value starts
        bool comment;
    };
    std::vector<Found> found;
    for (size_t at = 0; at < text.size(); at = next_line(text, at)) {
        const size_t end = std::min(text.find('\n', at), text.size());
        const std::string_view l(text.data() + at, end - at);
        std::string k;
        size_t value = 0;
        if (!setting_line(l, &k, &value) || k != key) continue;
        const bool comment = trim(l).starts_with("--");
        size_t stop = value_end(text, at + value);
        if (stop == std::string::npos || (comment && stop > end)) stop = end;  // (a commented-out line is one line)
        found.push_back({at, stop, comment});
    }
    // Its first line's indent.
    auto indent_of = [&](const Found& f) { return text.substr(f.start, std::min(text.find_first_not_of(" \t", f.start), f.stop) - f.start); };
    // Where more code after its value on that line starts (past a ";"), or
    // npos if there is none (only a comment, or nothing).
    auto more_code = [&](const Found& f, bool* semicolon) {
        const size_t eol = std::min(text.find('\n', f.stop), text.size());
        size_t at = std::min(text.find_first_not_of(" \t", f.stop), eol);
        *semicolon = at < eol && text[at] == ';';
        if (*semicolon) at = std::min(text.find_first_not_of(" \t", at + 1), eol);
        if (at == eol || text.compare(at, 2, "--") == 0 || text.compare(at, eol - at, "\r") == 0) return std::string::npos;
        return at;
    };
    auto replace_line = [&](const Found& f) {
        const size_t eol = std::min(text.find('\n', f.stop), text.size());
        const size_t line0 = f.stop == 0 ? 0 : text.rfind('\n', f.stop - 1) + 1;  // (its value's last line)
        const std::string rest = text.substr(f.stop, eol - f.stop);
        size_t c = rest.find_first_not_of(" \t");
        if (c != std::string::npos && rest[c] == ';') c = rest.find_first_not_of(" \t", c + 1);
        std::string now = indent_of(f) + line;
        bool semicolon = false;
        if (c != std::string::npos && rest.compare(c, 2, "--") == 0) {
            // (the comment where it was, if the line is no longer than that)
            const size_t col = f.stop + c - line0;
            now += now.size() + 1 < col ? std::string(col - now.size(), ' ') : std::string("  ");
            now += rest.substr(c);
        } else if (more_code(f, &semicolon) != std::string::npos) {
            now += rest;  // (another statement after it, as it was)
        }
        text.replace(f.start, eol - f.start, now);
    };
    if (line.empty()) {
        for (auto it = found.rbegin(); it != found.rend(); ++it) {
            if (it->comment) continue;
            // More code after it on its line goes to a line of its own, not
            // commented out with it.
            bool semicolon = false;
            if (const size_t more = more_code(*it, &semicolon); more != std::string::npos)
                text.replace(it->stop, more - it->stop, (semicolon ? ";\n" : "\n") + indent_of(*it));
            std::vector<size_t> starts{it->start};
            for (size_t nl = text.find('\n', it->start); nl < it->stop; nl = text.find('\n', nl + 1)) starts.push_back(nl + 1);
            for (auto s = starts.rbegin(); s != starts.rend(); ++s) text.insert(std::min(text.find_first_not_of(" \t", *s), text.size()), "-- ");
        }
        return text;
    }
    for (auto it = found.rbegin(); it != found.rend(); ++it)
        if (!it->comment) return replace_line(*it), text;
    for (const Found& f : found)
        if (f.comment) return replace_line(f), text;
    if (!text.empty() && text.back() != '\n') text += '\n';
    return text + line + "\n";
}

// The tab, or the one there is already.
void App::settings_open() {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->settings) {
                focus_pane(p.get());
                tab_show(i);
                panel_focus(false);
                settings_lay(*p->docs[i]);
                return;
            }
    // (In place of an empty tab with no name, as a file opened would be.)
    const bool blank = pane_->doc->ed.path.empty() && !pane_->doc->ed.modified() && pane_->doc->ed.buf.size() == 0 && !pane_->doc->read_only;
    const size_t was = pane_->tab;
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Settings";
    d->read_only = true;
    d->settings = std::make_shared<SettingsView>();
    d->settings->items = items_of_template();
    tab_show(pane_->tab + 1);
    if (blank) tab_close(was, true);
    panel_focus(false);
    settings_lay(*d);
    // (The cursor on the first setting.)
    for (u64 l = 0; l < d->settings->line_item.size(); ++l)
        if (const u32 i = d->settings->line_item[l]; i != ~0u && !d->settings->items[i].key.empty()) {
            d->ed.move_to(d->ed.buf.line_start(l), false);
            break;
        }
}

// The tab's text: a heading (in the accent), then its settings, a line
// each: the key, the value (one not the default in the added colour), and,
// muted, what it does.
void App::settings_lay(Doc& d) {
    SettingsView& sv = *d.settings;
    const u64 row = d.ed.buf.line_of(d.ed.cursor());
    static const Config defaults = make_config();
    size_t wide = 0;
    for (const SettingsView::Item& it : sv.items) wide = std::max(wide, it.key.size());
    std::string text;
    std::vector<u32> off;
    std::vector<u8> style;
    auto mark = [&](u8 st) {
        const u32 at = static_cast<u32>(text.size());
        if (!style.empty() && style.back() == st) return;
        if (!off.empty() && off.back() == at) style.back() = st;
        else off.push_back(at), style.push_back(st);
    };
    sv.line_item.clear();
    for (u32 i = 0; i < sv.items.size(); ++i) {
        const SettingsView::Item& it = sv.items[i];
        if (it.key.empty()) {
            if (!text.empty()) text += '\n', sv.line_item.push_back(~0u);
            mark(3);
            text += it.title + '\n';
            sv.line_item.push_back(i);
            continue;
        }
        mark(0);
        text += "  " + it.key;
        text.append(wide + 2 - it.key.size(), ' ');
        const std::string now = config_lua(config_get(cfg_, it.key));
        mark(now == config_lua(config_get(defaults, it.key)) ? 0 : 1);
        text += now;
        if (!it.about.empty()) {
            text.append(now.size() < 22 ? 22 - now.size() : 2, ' ');
            mark(4);
            text += it.about;
        }
        text += '\n';
        sv.line_item.push_back(i);
    }
    if (!text.empty()) text.pop_back();
    d.measurer = {};
    d.ed.open_bytes({}, text);
    d.ed.mark_saved();
    auto h = std::make_unique<Highlights>();
    h->base = 0;
    h->end = text.size();
    h->off = std::move(off);
    h->style = std::move(style);
    h->seal();
    d.marks = std::move(h);
    d.style_bits[1] = kInkAdded << kCellInkShift;
    d.style_bits[3] = kInkAccent << kCellInkShift;
    d.style_bits[4] = kInkMuted << kCellInkShift;
    d.widest_rev = d.indent_rev = ~0ull;
    d.ed.move_to(d.ed.buf.line_start(std::min<u64>(row, d.ed.buf.line_count() - 1)), false);
    dirty_ = true;
}

// Every settings tab made again (the settings have changed).
void App::settings_relay() {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (d->settings) settings_lay(*d);
}

// The setting under the cursor, or empty.
std::string App::settings_key_here() const {
    const Doc& d = *pane_->doc;
    if (!d.settings) return {};
    const u64 row = d.ed.buf.line_of(d.ed.cursor());
    if (row >= d.settings->line_item.size()) return {};
    const u32 i = d.settings->line_item[row];
    return i == ~0u ? std::string() : d.settings->items[i].key;
}

bool App::settings_key(const SDL_KeyboardEvent& k) {
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)) return false;
    if (cfg_.modal) {  // (not while a command or a search is typed)
        vim_attach();
        const Vim::Mode m = vim_.mode();
        if (m == Vim::kCommand || m == Vim::kInsert || m == Vim::kReplace || vim_.awaiting_char() || !vim_.showcmd().empty()) return false;
    }
    const std::string key = settings_key_here();
    switch (k.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!key.empty()) settings_change(key);
            break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            if (!key.empty()) settings_write(key, nullptr);
            break;
        case SDLK_O:
            if (k.mod & SDL_KMOD_SHIFT) return false;
            if (key.empty()) open_config();
            else open_setting(key);
            break;
        default: return false;
    }
    if (k.key == SDLK_SPACE || k.key == SDLK_O) term_drop_text_ = true;  // (only these make text, which would be typed; another's flag would eat the next key's)
    dirty_ = true;
    return true;
}

// A new value for it: a switch flipped; a theme picked from the themes;
// anything else typed (a list's items with commas between).
void App::settings_change(const std::string& key) {
    const ConfigValue now = config_get(cfg_, key);
    if (now.kind == ConfigValue::kBool) {
        ConfigValue v = now;
        v.flag = !v.flag;
        settings_write(key, &v);
        return;
    }
    if (key == "theme.dark" || key == "theme.light") {
        std::vector<std::string> names;
        for (const auto& [name, colours] : cfg_.themes) names.push_back(name);
        find_choose_item(key, names, [this, key, names](int i) {
            if (i < 0) return;
            ConfigValue v;
            v.kind = ConfigValue::kText;
            v.text = names[static_cast<size_t>(i)];
            settings_write(key, &v);
        });
        return;
    }
    find_choose(kFindInput, key, typed_of(now), [this, key, kind = now.kind](const std::string& typed) {
        ConfigValue v;
        v.kind = kind;
        const std::string_view t = trim(typed);
        if (kind == ConfigValue::kNumber) {
            char* end = nullptr;
            const std::string s(t);
            v.number = std::strtod(s.c_str(), &end);
            if (s.empty() || *end) return notify(key + ": not a number", kError);
        } else if (kind == ConfigValue::kList) {
            for (size_t at = 0; at <= t.size();) {
                size_t comma = t.find(',', at);
                if (comma == std::string_view::npos) comma = t.size();
                if (const std::string_view item = trim(t.substr(at, comma - at)); !item.empty()) v.list.emplace_back(item);
                at = comma + 1;
            }
        } else {
            v.text = std::string(t);
        }
        settings_write(key, &v);
    });
}

// The setting made (nullptr: back to its default) in init.lua, and that
// run again.
void App::settings_write(const std::string& key, const ConfigValue* v) {
    std::string line;
    if (v) {
        Config probe = cfg_;
        if (const std::string wrong = config_set(probe, {key, literal(*v), "Settings"}); !wrong.empty()) return notify(wrong, kError);
        line = "blit.opt." + key + " = " + config_lua(*v);
    }
    // init.lua open in a tab: the line put there (and saved, if it had no
    // changes of its own).
    Doc* open = nullptr;
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (!d->ed.path.empty() && !d->read_only && full_path(d->ed.path) == full_path(init_path_)) open = d.get();
    if (open) {
        Editor& ed = open->ed;
        const bool changes = ed.modified();
        const std::string was = ed.buf.text(0, ed.buf.size());
        const std::string now = init_with(was, key, line);
        if (now == was) return;
        size_t a = 0;
        while (a < was.size() && a < now.size() && was[a] == now[a]) ++a;
        size_t b = 0;
        while (b < was.size() - a && b < now.size() - a && was[was.size() - 1 - b] == now[now.size() - 1 - b]) ++b;
        ed.replace(a, was.size() - b, now.substr(a, now.size() - a - b));
        if (changes) return notify("init.lua has changes not saved: the setting is put there, to be saved with them");
        std::string err;
        if (!ed.save(&err)) return tell(SDL_MESSAGEBOX_ERROR, err.c_str());
        open->disk_stamp = stamp_of(full_path(ed.path));
    } else {
        std::string was;
        std::string why;
        if (!fs_read_whole(init_path_, &was, &why, 16u << 20, {})) was = std::string(init_template());
        const std::string now = init_with(was, key, line);
        if (now == was) return;
        const size_t slash = init_path_.find_last_of('/');
        if (slash != std::string::npos) SDL_CreateDirectory(init_path_.substr(0, slash).c_str());
        if (!SDL_SaveFile(init_path_.c_str(), now.data(), now.size())) return tell(SDL_MESSAGEBOX_ERROR, SDL_GetError());
    }
    script_restart();  // (which makes the settings tabs again)
    if (v && config_lua(config_get(cfg_, key)) != config_lua(*v)) notify(key + " is set again further on in init.lua, or by a plugin");
}

// For the tests: the setting under the cursor and its value as shown.
std::string App::settings_dump() {
    const Doc& d = *pane_->doc;
    if (!d.settings) return "no settings tab";
    const u64 row = d.ed.buf.line_of(d.ed.cursor());
    const u64 a = d.ed.buf.line_start(row);
    return "settings: " + std::to_string(d.settings->items.size()) + " items, line: " + d.ed.buf.text(a, d.ed.line_end(row) - a);
}

}  // namespace blit
