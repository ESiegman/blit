#include "config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace blit {

namespace {

// The keys for git's blame column, for a message (this file knows nothing of SDL).
#if defined(__APPLE__)
constexpr const char* kBlameKeys = "Cmd+Option+B";
#else
constexpr const char* kBlameKeys = "Ctrl+Alt+B";
#endif

constexpr Rgb hex(u32 v) {
    return {static_cast<float>((v >> 16) & 255) / 255.0f, static_cast<float>((v >> 8) & 255) / 255.0f,
            static_cast<float>(v & 255) / 255.0f};
}

using Syn = std::initializer_list<std::pair<const char*, u32>>;
using Sty = std::initializer_list<std::pair<const char*, const char*>>;

void set_all(ThemeColors& t, Syn list) {
    for (const auto& [name, rgb] : list) t.set_syntax(name, hex(rgb));
}

// Italic and bold, in the words the config file uses.
void style_all(ThemeColors& t, Sty list) {
    for (const auto& [name, words] : list) {
        ThemeColors::Entry e;
        e.capture = name;
        parse_token_style(words, &e);
        t.set_syntax(e);
    }
}

// Rosé Pine (https://rosepinetheme.com). Interface: background is "base",
// text is "text", selection is "highlight med", the cursor is "rose", the
// scroll bar is "muted". Syntax follows the palette's stated roles: pine for
// keywords, foam for types and properties, rose for functions and booleans,
// gold for strings and constants, iris for parameters and preprocessor, love
// for builtin variables, subtle for comments, operators and punctuation.
// Italic and bold as the theme's editor ports have them: italic comments,
// variables, parameters and properties; bold builtins and headings.
// Indent line colours for a theme: each given colour, pulled most of the way
// toward the background so that the lines sit behind the text.
std::vector<Rgb> guide_colours(u32 background, std::initializer_list<u32> colours) {
    const Rgb bg = hex(background);
    std::vector<Rgb> out;
    for (const u32 c : colours) {
        const Rgb x = hex(c);
        constexpr float k = 0.45f;
        out.push_back({bg.r + (x.r - bg.r) * k, bg.g + (x.g - bg.g) * k, bg.b + (x.b - bg.b) * k});
    }
    return out;
}

ThemeColors rose_pine(u32 base, u32 surface, u32 text, u32 highlight_med, u32 highlight_high, u32 muted, u32 subtle,
                      u32 love, u32 gold, u32 rose, u32 pine, u32 foam, u32 iris) {
    ThemeColors t{};
    t.background = hex(base);
    t.foreground = hex(text);
    t.selection = hex(highlight_med);
    t.cursor = hex(rose);
    t.scrollbar = hex(muted);
    // Diagnostics as the palette assigns them: love for errors, gold for
    // warnings, foam for information, iris for hints. Popups sit on
    // "surface" with a "highlight high" outline.
    t.error = hex(love);
    t.warning = hex(gold);
    t.info = hex(foam);
    t.hint = hex(iris);
    t.muted = hex(muted);
    t.accent = hex(rose);
    t.popup = hex(surface);
    t.popup_border = hex(highlight_high);
    t.guides = guide_colours(base, {rose, gold, foam, iris, pine, love});
    t.brackets = {hex(gold), hex(iris), hex(foam)};
    set_all(t, {
        {"comment", subtle}, {"comment.error", love}, {"comment.warning", gold}, {"comment.todo", rose}, {"comment.note", pine},
        {"string", gold}, {"string.escape", pine}, {"string.regexp", iris}, {"string.special.url", iris},
        {"character", gold}, {"number", gold}, {"boolean", rose},
        {"constant", gold}, {"function", rose}, {"constructor", foam},
        {"keyword", pine}, {"keyword.operator", subtle}, {"keyword.storage", foam}, {"keyword.directive", iris},
        {"operator", subtle}, {"punctuation", subtle},
        {"type", foam}, {"attribute", iris}, {"property", foam},
        {"variable", text}, {"variable.builtin", love}, {"variable.parameter", iris}, {"variable.member", foam},
        {"module", text}, {"label", foam},
        {"tag", foam}, {"tag.attribute", iris}, {"tag.delimiter", subtle},
        {"markup.heading", iris}, {"markup.link", iris}, {"markup.raw", foam}, {"markup.list", pine},
        {"markup.quote", subtle}, {"markup.math", foam},
        {"diff.plus", foam}, {"diff.minus", love}, {"diff.delta", rose},
    });
    style_all(t, {
        {"comment", "italic"}, {"variable", "italic"}, {"variable.member", "regular"}, {"variable.builtin", "bold"},
        {"variable.parameter.builtin", "bold"}, {"property", "italic"},
        {"constant.builtin", "bold"}, {"module.builtin", "bold"}, {"type.builtin", "bold"},
        {"attribute.builtin", "bold"}, {"function.builtin", "bold"},
        {"markup.italic", "italic"}, {"markup.strong", "bold"}, {"markup.heading", "bold"},
        {"markup.link.url", "underline"}, {"markup.strikethrough", "strikethrough"},
    });
    return t;
}

void add_builtin_themes(Config& c) {
    auto base = [](u32 bg, u32 fg, u32 sel, u32 cursor, u32 bar, u32 error, u32 warning, u32 info, u32 hint, u32 muted,
                   u32 accent, u32 popup, u32 border) {
        ThemeColors t{};
        t.background = hex(bg);
        t.foreground = hex(fg);
        t.selection = hex(sel);
        t.cursor = hex(cursor);
        t.scrollbar = hex(bar);
        t.error = hex(error);
        t.warning = hex(warning);
        t.info = hex(info);
        t.hint = hex(hint);
        t.muted = hex(muted);
        t.accent = hex(accent);
        t.popup = hex(popup);
        t.popup_border = hex(border);
        return t;
    };
    //                          background foreground selection cursor  scrollbar error     warning   info      hint      muted     accent    popup     border
    c.themes["blit-dark"] = base(0x1c1d21, 0xdbdedb, 0x304f80, 0xffffff, 0x7b7e86, 0xe06c75, 0xe5c07b, 0x61afef, 0x56b6c2, 0x7f848e, 0x61afef, 0x26282e, 0x41444d);
    set_all(c.themes["blit-dark"], {
        {"comment", 0x7f848e}, {"string", 0x98c379}, {"string.escape", 0x56b6c2}, {"string.regexp", 0x56b6c2},
        {"character", 0x98c379}, {"number", 0xd19a66}, {"boolean", 0xd19a66}, {"constant", 0xd19a66},
        {"function", 0x61afef}, {"constructor", 0xe5c07b}, {"keyword", 0xc678dd}, {"keyword.operator", 0xc678dd},
        {"operator", 0x9aa2b1}, {"punctuation", 0x9aa2b1}, {"type", 0xe5c07b}, {"attribute", 0xd19a66},
        {"property", 0xe06c75}, {"variable.builtin", 0xe06c75}, {"variable.member", 0xe06c75},
        {"module", 0xe5c07b}, {"label", 0xe06c75}, {"tag", 0xe06c75}, {"tag.attribute", 0xd19a66},
        {"tag.delimiter", 0x9aa2b1}, {"markup.heading", 0xe06c75}, {"markup.link", 0x61afef}, {"markup.raw", 0x98c379},
        {"markup.list", 0xc678dd}, {"markup.quote", 0x7f848e}, {"diff.plus", 0x98c379}, {"diff.minus", 0xe06c75},
        {"diff.delta", 0xe5c07b},
    });
    c.themes["blit-dark"].guides = guide_colours(0x1c1d21, {0x61afef, 0xe5c07b, 0x98c379, 0xc678dd, 0xe06c75, 0x56b6c2});
    c.themes["blit-dark"].brackets = {hex(0xe5c07b), hex(0xc678dd), hex(0x61afef)};
    const Sty plain_styles = {{"comment", "italic"}, {"markup.italic", "italic"}, {"markup.strong", "bold"},
                              {"markup.heading", "bold"}, {"markup.link.url", "underline"}, {"markup.strikethrough", "strikethrough"}};
    style_all(c.themes["blit-dark"], plain_styles);
    c.themes["blit-light"] = base(0xffffff, 0x1a1a1c, 0xb3d4ff, 0x000000, 0x9a9aa0, 0xca3c2c, 0xa87b00, 0x325cc0, 0x0184bc, 0x75777e, 0x325cc0, 0xf3f3f5, 0xcfd0d6);
    set_all(c.themes["blit-light"], {
        {"comment", 0x75777e}, {"string", 0x448c27}, {"string.escape", 0x0184bc}, {"string.regexp", 0x0184bc},
        {"character", 0x448c27}, {"number", 0x986801}, {"boolean", 0x986801}, {"constant", 0x986801},
        {"function", 0x325cc0}, {"constructor", 0xa87b00}, {"keyword", 0xa626a4}, {"keyword.operator", 0xa626a4},
        {"operator", 0x5c5f69}, {"punctuation", 0x5c5f69}, {"type", 0xa87b00}, {"attribute", 0x986801},
        {"property", 0xca3c2c}, {"variable.builtin", 0xca3c2c}, {"variable.member", 0xca3c2c},
        {"module", 0xa87b00}, {"label", 0xca3c2c}, {"tag", 0xca3c2c}, {"tag.attribute", 0x986801},
        {"tag.delimiter", 0x5c5f69}, {"markup.heading", 0xca3c2c}, {"markup.link", 0x325cc0}, {"markup.raw", 0x448c27},
        {"markup.list", 0xa626a4}, {"markup.quote", 0x75777e}, {"diff.plus", 0x448c27}, {"diff.minus", 0xca3c2c},
        {"diff.delta", 0xa87b00},
    });
    c.themes["blit-light"].guides = guide_colours(0xffffff, {0x325cc0, 0xa87b00, 0x448c27, 0xa626a4, 0xca3c2c, 0x0184bc});
    c.themes["blit-light"].brackets = {hex(0xa87b00), hex(0xa626a4), hex(0x325cc0)};
    style_all(c.themes["blit-light"], plain_styles);
    //                                  base      surface   text      hl med    hl high   muted     subtle    love      gold      rose      pine      foam      iris
    c.themes["rose-pine"] = rose_pine(0x191724, 0x1f1d2e, 0xe0def4, 0x403d52, 0x524f67, 0x6e6a86, 0x908caa, 0xeb6f92, 0xf6c177, 0xebbcba, 0x31748f, 0x9ccfd8, 0xc4a7e7);
    c.themes["rose-pine-moon"] = rose_pine(0x232136, 0x2a273f, 0xe0def4, 0x44415a, 0x56526e, 0x6e6a86, 0x908caa, 0xeb6f92, 0xf6c177, 0xea9a97, 0x3e8fb0, 0x9ccfd8, 0xc4a7e7);
    c.themes["rose-pine-dawn"] = rose_pine(0xfaf4ed, 0xfffaf3, 0x575279, 0xdfdad9, 0xcecacd, 0x9893a5, 0x797593, 0xb4637a, 0xea9d34, 0xd7827e, 0x286983, 0x56949f, 0x907aa9);
    // (Dawn's muted is 2.7:1 on its base: secondary text, line numbers and
    // the status bar take the palette's next step, subtle, 4:1.)
    c.themes["rose-pine-dawn"].muted = hex(0x797593);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Reads one quoted string from the front of s and removes it from s.
bool take_string(std::string_view& s, std::string* out) {
    s = trim(s);
    if (s.empty() || (s[0] != '"' && s[0] != '\'')) return false;
    const char quote = s[0];
    out->clear();
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == quote) {
            s.remove_prefix(i + 1);
            return true;
        }
        if (c == '\\' && quote == '"' && i + 1 < s.size()) {
            const char e = s[++i];
            *out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
            *out += c;
        }
    }
    return false;  // no closing quote
}

bool as_string(std::string_view v, std::string* out) {
    return take_string(v, out) && trim(v).empty();
}

bool as_number(std::string_view v, double* out) {
    const std::string s(trim(v));
    if (s.empty()) return false;
    char* end = nullptr;
    *out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

bool as_bool(std::string_view v, bool* out) {
    v = trim(v);
    if (v == "true") *out = true;
    else if (v == "false") *out = false;
    else return false;
    return true;
}

bool as_string_array(std::string_view v, std::vector<std::string>* out) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
    v = trim(v.substr(1, v.size() - 2));
    out->clear();
    while (!v.empty()) {
        std::string item;
        if (!take_string(v, &item)) return false;
        out->push_back(std::move(item));
        v = trim(v);
        if (v.empty()) break;
        if (v.front() != ',') return false;
        v = trim(v.substr(1));
    }
    return true;
}

// One setting at a time (see make_config() and config_set()): its key, its
// value as a literal ("14", "\"text\"", "true", "[\"a\", \"b\"]"), and
// where it was made, for the warning if it is wrong.
struct Parser {
    Config& c;
    std::string from;
    std::vector<ThemeColors::Entry> everywhere;                         // syntax.styles.*
    std::vector<std::pair<std::string, ThemeColors::Entry>> per_theme;  // themes.NAME.syntax.*

    void warn(const std::string& what) { c.warnings += (from.empty() ? std::string("init.lua") : from) + ": " + what + "\n"; }

    void text(std::string_view key, std::string_view v, std::string* out) {
        if (!as_string(v, out)) warn(std::string(key) + " should be a quoted string");
    }
    void number(std::string_view key, std::string_view v, double lo, double hi, auto* out) {
        double d;
        if (!as_number(v, &d) || !(d >= lo && d <= hi)) {
            // (Whole numbers as they are, others with no trailing zeros.)
            auto shown = [](double x) {
                char b[32];
                std::snprintf(b, sizeof b, "%g", x);
                return std::string(b);
            };
            warn(std::string(key) + " should be a number from " + shown(lo) + " to " + shown(hi));
            return;
        }
        *out = static_cast<std::remove_pointer_t<decltype(out)>>(d);
    }
    void colour(std::string_view key, std::string_view v, Rgb* out) {
        std::string s;
        if (!as_string(v, &s) || !parse_color(s, out)) warn(std::string(key) + " should be a colour like \"#1a2b3c\"");
    }

    bool token_style(std::string_view key, std::string_view capture, std::string_view v, ThemeColors::Entry* e) {
        std::string str;
        if (capture.empty() || !as_string(v, &str) || !parse_token_style(str, e)) {
            warn(std::string(key) + " should be a colour and/or style like \"#1a2b3c italic\"");
            return false;
        }
        e->capture = capture;
        return true;
    }

    // Token styles go in once the whole file is read: first the ones for
    // every theme, then each theme's own, so the more specific one wins
    // wherever the two tables sit in the file.
    void finish() {
        for (auto& [name, theme] : c.themes)
            for (const ThemeColors::Entry& e : everywhere) theme.set_syntax(e);
        for (const auto& [name, e] : per_theme) c.themes.find(name)->second.set_syntax(e);
    }

    void set(std::string_view key, std::string_view v) {
        if (key == "font.family") return text(key, v, &c.font_family);
        if (key == "font.style") return text(key, v, &c.font_style);
        if (key == "font.size") return number(key, v, 4, 200, &c.font_size);
        if (key == "font.line_height") return number(key, v, 0.5, 4, &c.line_height);
        if (key == "font.ligatures") {
            if (!as_bool(v, &c.ligatures)) warn("font.ligatures should be true or false");
            return;
        }
        if (key == "font.features") {
            if (!as_string_array(v, &c.font_features)) warn("font.features should be a list like [\"ss01\", \"zero\"]");
            return;
        }
        if (key == "theme") {  // one theme for both modes
            text(key, v, &c.theme_dark);
            c.theme_light = c.theme_dark;
            return;
        }
        if (key == "theme.dark") return text(key, v, &c.theme_dark);
        if (key == "theme.light") return text(key, v, &c.theme_light);
        if (key == "editor.tab_width") return number(key, v, 1, 32, &c.tab_width);
        if (key == "editor.indent_width") return number(key, v, 0, 32, &c.indent_width);
        if (key == "editor.scrolloff") return number(key, v, 0, 999, &c.scrolloff);
        if (key == "remote.agent_idle") return number(key, v, 1, 1440, &c.agent_idle);
        if (key == "remote.agent_keep") return number(key, v, 1, 720, &c.agent_keep);
        if (key == "look.presentation_size") {
            double n = 0;
            if (!as_number(v, &n) || n < 0 || n > 96) return warn("look.presentation_size should be a size from 6 to 96, or 0");
            c.presentation_size = static_cast<float>(n);
            return;
        }
        if (key == "editor.line_numbers" || key == "editor.relative_numbers" || key == "editor.indent_guides" || key == "editor.tab_bar" || key == "editor.status_bar" || key == "editor.start_screen" ||
            key == "editor.restore" || key == "scroll.momentum" || key == "editor.modal" || key == "editor.breadcrumbs" || key == "editor.sticky_scroll" || key == "editor.close_pairs" || key == "editor.local_history" || key == "editor.minimap" || key == "editor.spell_check" || key == "editor.soft_wrap" || key == "panel.watch" || key == "updates.check") {
            bool* out = key == "editor.modal"          ? &c.modal
                        : key == "editor.line_numbers" ? &c.line_numbers
                        : key == "editor.relative_numbers" ? &c.relative_numbers
                        : key == "editor.indent_guides" ? &c.indent_guides
                        : key == "editor.tab_bar"       ? &c.tab_bar
                        : key == "editor.status_bar"    ? &c.status_bar
                        : key == "editor.start_screen"  ? &c.start_screen
                        : key == "editor.restore"       ? &c.restore
                        : key == "editor.breadcrumbs"   ? &c.breadcrumbs
                        : key == "editor.sticky_scroll" ? &c.sticky_scroll
                        : key == "editor.close_pairs"   ? &c.close_pairs
                        : key == "editor.local_history" ? &c.local_history
                        : key == "editor.minimap"       ? &c.minimap
                        : key == "editor.spell_check"   ? &c.spell
                        : key == "editor.soft_wrap"     ? &c.soft_wrap
                        : key == "panel.watch"          ? &c.watch
                        : key == "updates.check"        ? &c.update_check
                                                        : &c.scroll_momentum;
            if (!as_bool(v, out)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "build.notify") {
            if (!as_bool(v, &c.build_notify)) warn("build.notify should be true or false");
            return;
        }
        if (key == "serial.baud") return number(key, v, 50, 20'000'000, &c.serial_baud);
        if (key == "serial.line_end") {
            std::string s;
            if (!as_string(v, &s) || (s != "cr" && s != "lf" && s != "crlf")) return warn("serial.line_end should be \"cr\", \"lf\" or \"crlf\"");
            c.serial_line_end = s;
            return;
        }
        if (key == "rtt.line_end") {
            std::string s;
            if (!as_string(v, &s) || (s != "cr" && s != "lf" && s != "crlf")) return warn("rtt.line_end should be \"cr\", \"lf\" or \"crlf\"");
            c.rtt_line_end = s;
            return;
        }
        if (key == "timeline.max_mb") return number(key, v, 16, 1 << 20, &c.timeline_max_mb);
        if (key == "wave.remote_fetch_mb") return number(key, v, 1, 1 << 20, &c.wave_remote_fetch_mb);
        if (key == "syntax.enabled") {
            if (!as_bool(v, &c.syntax)) warn("syntax.enabled should be true or false");
            return;
        }
        if (key == "syntax.max_file_mb") return number(key, v, 0, 2047, &c.syntax_max_mb);
        if (key == "editor.soft_wrap_languages") {
            if (!as_string_array(v, &c.soft_wrap_languages)) warn("editor.soft_wrap_languages should be a list like [\"markdown\", \"text\"]");
            return;
        }
        if (key == "syntax.parser_dirs" || key == "syntax.query_dirs") {
            if (!as_string_array(v, key == "syntax.parser_dirs" ? &c.parser_dirs : &c.query_dirs))
                warn(std::string(key) + " should be a list of folders");
            return;
        }
        if (key == "lsp.enabled" || key == "lsp.completion" || key == "lsp.inline_diagnostics" || key == "lsp.format_on_save" ||
            key == "lsp.inlay_hints" || key == "lsp.semantic_colors" || key == "lsp.highlight_uses" || key == "lsp.reference_counts") {
            bool* out = key == "lsp.enabled"          ? &c.lsp
                        : key == "lsp.completion"     ? &c.lsp_completion
                        : key == "lsp.format_on_save" ? &c.lsp_format_on_save
                        : key == "lsp.inlay_hints"    ? &c.lsp_inlay_hints
                        : key == "lsp.semantic_colors" ? &c.lsp_semantic
                        : key == "lsp.highlight_uses" ? &c.lsp_uses
                        : key == "lsp.reference_counts" ? &c.lsp_ref_counts
                                                      : &c.lsp_inline;
            if (!as_bool(v, out)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "lsp.max_file_mb") return number(key, v, 0, 2047, &c.lsp_max_mb);
        if (key == "lsp.hover_ms") return number(key, v, 0, 10000, &c.lsp_hover_ms);
        if (key.starts_with("lsp.servers.")) {
            // lsp.servers.NAME.FIELD: how to run one language server. A
            // built-in server of that name is the starting point.
            // (language_id is a string, or a table by language:
            // lsp.servers.NAME.language_id.LANGUAGE.)
            const size_t ids = key.find(".language_id.", 12);
            const size_t dot = ids != std::string_view::npos ? ids : key.rfind('.');
            const std::string_view name = dot > 12 ? key.substr(12, dot - 12) : std::string_view{}, field = key.substr(dot + 1);
            if (name.empty()) return warn("language servers go under [lsp.servers.NAME]");
            LspServer* s = nullptr;
            for (LspServer& have : c.lsp_servers)
                if (have.name == name) s = &have;
            if (!s) {
                c.lsp_servers.push_back({});
                s = &c.lsp_servers.back();
                s->name = name;
            }
            if (field == "command" || field == "languages" || field == "roots") {
                std::vector<std::string>* list = field == "command" ? &s->command : field == "languages" ? &s->languages : &s->roots;
                if (!as_string_array(v, list)) warn(std::string(key) + " should be a list like [\"clangd\", \"--log=error\"]");
                return;
            }
            if (field == "enabled") {
                if (!as_bool(v, &s->enabled)) warn(std::string(key) + " should be true or false");
                return;
            }
            if (field == "init_options") return text(key, v, &s->init_options);
            if (field == "settings") return text(key, v, &s->settings);
            if (field == "language_id" || ids != std::string_view::npos) {
                const std::string language(ids != std::string_view::npos ? key.substr(ids + 13) : std::string_view{});
                std::string id;
                if (!as_string(v, &id) || id.empty())
                    return warn(std::string(key) + " should be a name like \"cuda-cpp\", or a table of them by language");
                if (language.empty()) s->language_ids.clear();  // (one for every language)
                s->language_ids[language] = id;
                return;
            }
        }
        if (key == "git.enabled") {
            if (!as_bool(v, &c.git)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "python.interpreter") return text(key, v, &c.python_interpreter);
        if (key == "debug.inline_values") {
            if (!as_bool(v, &c.debug_inline_values)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "git.blame") return warn(std::string("git.blame is no longer a setting: blame shows in a file when ") + kBlameKeys + " or the Git tab's Blame turns it on");
        if (key == "panel.open") {
            if (!as_bool(v, &c.panel_open)) warn("panel.open should be true or false");
            return;
        }
        if (key == "panel.width") return number(key, v, 12, 200, &c.panel_width);
        if (key == "scroll.lines") return number(key, v, 0.1, 100, &c.scroll_lines);
        if (key == "scroll.ease_ms") return number(key, v, 0, 2000, &c.scroll_ease_ms);
        if (key == "search.ignore") {
            if (!as_string_array(v, &c.search_ignore)) warn("search.ignore should be a list like [\"*.log\", \"/dist/\"]");
            return;
        }
        if (key == "search.gitignore" || key == "search.hidden") {
            if (!as_bool(v, key == "search.gitignore" ? &c.search_gitignore : &c.search_hidden)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "markdown.web_pictures") {
            if (!as_bool(v, &c.markdown_web_pictures)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "look.shadows" || key == "look.animations") {
            if (!as_bool(v, key == "look.shadows" ? &c.look_shadows : &c.look_animations)) warn(std::string(key) + " should be true or false");
            return;
        }
        if (key == "look.rounding") return number(key, v, 0, 16, &c.look_rounding);
        if (key == "look.cursor_glide_ms") return number(key, v, 0, 1000, &c.look_cursor_glide_ms);
        if (key == "look.dim_inactive") return number(key, v, 0, 0.6, &c.look_dim_inactive);
        if (key == "window.width") return number(key, v, 200, 16000, &c.window_width);
        if (key == "window.height") return number(key, v, 100, 16000, &c.window_height);
        if (key == "keys.timeout_ms") return number(key, v, 0, 60000, &c.key_timeout_ms);
        if (key == "keys.leader") return text(key, v, &c.key_leader);
        if (key == "script.timeout_ms") return number(key, v, 100, 600000, &c.script_timeout_ms);

        if (key == "syntax.install") {
            if (!as_string_array(v, &c.syntax_install)) warn("syntax.install should be a list like [\"cpp\", \"python\"]");
            return;
        }
        if (key == "syntax.auto_install") {
            if (!as_bool(v, &c.syntax_auto_install)) warn("syntax.auto_install should be true or false");
            return;
        }
        if (key.starts_with("syntax.grammars.")) {
            // syntax.grammars.NAME.FIELD: where one grammar comes from.
            const size_t dot = key.rfind('.');
            const std::string_view name = dot > 16 ? key.substr(16, dot - 16) : std::string_view{}, field = key.substr(dot + 1);
            if (name.empty()) return warn("grammars go under [syntax.grammars.NAME]");
            GrammarSource* g = nullptr;
            for (GrammarSource& have : c.grammars)
                if (have.name == name) g = &have;
            if (!g) {
                c.grammars.push_back({});
                g = &c.grammars.back();
                g->name = name;
            }
            if (field == "repo") return text(key, v, &g->repo);
            if (field == "rev") return text(key, v, &g->rev);
            if (field == "path") return text(key, v, &g->path);
            if (field == "parser") return text(key, v, &g->parser);
            if (field == "comment") return text(key, v, &g->comment);
            if (field == "name") return text(key, v, &g->display);
            if (field == "filenames") {
                std::vector<std::string> list;
                if (!as_string_array(v, &list)) return warn(std::string(key) + " should be a list like [\"Kconfig\", \"*_defconfig\"]");
                for (const std::string& f : list)
                    if (f.empty() || f.find_first_of("/\\") != std::string::npos)
                        return warn(std::string(key) + ": \"" + f + "\" is not a file's name like \"Kconfig\" or \"*_defconfig\"");
                g->filenames = std::move(list);
                return;
            }
            if (field == "inherits") {
                if (!as_string_array(v, &g->inherits)) warn(std::string(key) + " should be a list like [\"c\"]");
                return;
            }
            if (field == "extensions") {
                // Kept as file endings in small letters, without the dot,
                // the way the file's name is looked up.
                std::vector<std::string> list;
                if (!as_string_array(v, &list)) return warn(std::string(key) + " should be a list like [\"rs\"]");
                for (std::string& e : list) {
                    if (e.starts_with('.')) e.erase(0, 1);
                    for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (e.empty() || e.find_first_of("./\\ ") != std::string::npos)
                        return warn(std::string(key) + ": \"" + e + "\" is not a file ending like \"rs\"");
                }
                g->extensions = std::move(list);
                return;
            }
        }
        if (key == "syntax.italic" || key == "syntax.bold" || key == "syntax.bracket_colors") {
            if (!as_bool(v, key == "syntax.italic" ? &c.syntax_italic : key == "syntax.bold" ? &c.syntax_bold : &c.syntax_brackets))
                warn(std::string(key) + " should be true or false");
            return;
        }
        if (key.starts_with("syntax.styles.")) {
            // How one kind of token is drawn in every theme.
            ThemeColors::Entry e;
            if (token_style(key, key.substr(14), v, &e)) everywhere.push_back(std::move(e));
            return;
        }
        if (const size_t syn = key.find(".syntax."); key.starts_with("themes.") && syn != std::string_view::npos && syn > 7) {
            // themes.NAME.syntax.CAPTURE: how one kind of token is drawn.
            const std::string_view name = key.substr(7, syn - 7);
            auto it = c.themes.find(name);
            if (it == c.themes.end()) it = c.themes.emplace(std::string(name), c.themes.find("blit-dark")->second).first;
            ThemeColors::Entry e;
            if (token_style(key, key.substr(syn + 8), v, &e)) per_theme.emplace_back(it->first, std::move(e));
            return;
        }
        if (key.starts_with("themes.")) {
            // themes.NAME.colour: a new theme, or changes to a built-in one.
            const size_t dot = key.rfind('.');
            const std::string_view name = key.substr(7, dot > 7 ? dot - 7 : 0), field = key.substr(dot + 1);
            if (name.empty()) return warn("theme colours go under [themes.NAME]");
            auto it = c.themes.find(name);
            if (it == c.themes.end()) it = c.themes.emplace(std::string(name), c.themes.find("blit-dark")->second).first;
            ThemeColors& t = it->second;
            if (field == "background") return colour(key, v, &t.background);
            if (field == "foreground") return colour(key, v, &t.foreground);
            if (field == "selection") return colour(key, v, &t.selection);
            if (field == "cursor") return colour(key, v, &t.cursor);
            if (field == "scrollbar") return colour(key, v, &t.scrollbar);
            if (field == "error") return colour(key, v, &t.error);
            if (field == "warning") return colour(key, v, &t.warning);
            if (field == "info") return colour(key, v, &t.info);
            if (field == "hint") return colour(key, v, &t.hint);
            if (field == "muted") return colour(key, v, &t.muted);
            if (field == "accent") return colour(key, v, &t.accent);
            if (field == "popup") return colour(key, v, &t.popup);
            if (field == "popup_border") return colour(key, v, &t.popup_border);
            if (field == "guides") {
                std::vector<std::string> names;
                std::vector<Rgb> colours;
                bool ok = as_string_array(v, &names) && !names.empty() && names.size() <= 6;
                for (const std::string& n : names) {
                    Rgb rgb{};
                    ok = ok && parse_color(n, &rgb);
                    colours.push_back(rgb);
                }
                if (!ok) return warn(std::string(key) + " should be a list of one to six colours like [\"#eb6f92\", \"#f6c177\"]");
                t.guides = std::move(colours);
                return;
            }
            if (field == "brackets") {
                std::vector<std::string> names;
                std::vector<Rgb> colours;
                bool ok = as_string_array(v, &names) && names.size() <= 6;
                for (const std::string& n : names) {
                    Rgb rgb{};
                    ok = ok && parse_color(n, &rgb);
                    colours.push_back(rgb);
                }
                if (!ok) return warn(std::string(key) + " should be a list of up to six colours like [\"#f6c177\", \"#c4a7e7\"]");
                t.brackets = std::move(colours);
                return;
            }
        }
        warn("unknown setting " + std::string(key));
    }
};

}  // namespace

bool parse_color(std::string_view s, Rgb* out) {
    s = trim(s);
    if (s.empty() || s[0] != '#' || (s.size() != 4 && s.size() != 7)) return false;
    u32 v = 0;
    for (char ch : s.substr(1)) {
        const u32 d = ch >= '0' && ch <= '9'   ? static_cast<u32>(ch - '0')
                      : ch >= 'a' && ch <= 'f' ? static_cast<u32>(ch - 'a' + 10)
                      : ch >= 'A' && ch <= 'F' ? static_cast<u32>(ch - 'A' + 10)
                                               : 99u;
        if (d > 15) return false;
        v = s.size() == 4 ? (v << 8) | (d * 17) : (v << 4) | d;  // "#abc" means "#aabbcc"
    }
    *out = hex(v);
    return true;
}

bool parse_token_style(std::string_view s, ThemeColors::Entry* out) {
    bool any = false;
    for (s = trim(s); !s.empty(); s = trim(s)) {
        const size_t sp = s.find_first_of(" \t,");
        const std::string_view word = s.substr(0, sp);
        s = sp == std::string_view::npos ? std::string_view{} : s.substr(sp + 1);
        if (word.empty()) continue;
        if (word == "italic") out->italic = 1;
        else if (word == "bold") out->bold = 1;
        else if (word == "underline") out->underline = 1;
        else if (word == "strikethrough" || word == "strike") out->strike = 1;
        else if (word == "regular") out->italic = out->bold = out->underline = out->strike = 0;
        else if (parse_color(word, &out->color)) out->has_color = true;
        else return false;
        any = true;
    }
    return any;
}

TokenStyle ThemeColors::syntax_style(std::string_view capture) const {
    const std::string_view asked = capture;
    TokenStyle out{foreground, false, false};
    bool color = false, italic = false, bold = false, underline = false, strike = false;  // which of them are settled
    bool found = false;
    for (;;) {
        for (const Entry& e : syntax) {
            if (e.capture != capture) continue;
            found = true;
            if (!color && e.has_color) out.color = e.color, color = true;
            if (!italic && e.italic >= 0) out.italic = e.italic != 0, italic = true;
            if (!bold && e.bold >= 0) out.bold = e.bold != 0, bold = true;
            if (!underline && e.underline >= 0) out.underline = e.underline != 0, underline = true;
            if (!strike && e.strike >= 0) out.strike = e.strike != 0, strike = true;
            break;
        }
        const size_t dot = capture.rfind('.');
        if (dot == std::string_view::npos || (color && italic && bold && underline && strike)) break;
        capture = capture.substr(0, dot);
    }
    if (found) return out;
    // Highlight queries do not all use the same names for the same things:
    // some say "field" where others say "property", or "repeat" for a loop
    // keyword. A name the theme says nothing about is read as the usual
    // one for it.
    static const struct {
        std::string_view name, same_as;
    } other_names[] = {
        {"delimiter", "punctuation"},     {"escape", "string.escape"},  {"repeat", "keyword.repeat"},
        {"conditional", "keyword.conditional"}, {"include", "keyword.import"}, {"exception", "keyword.exception"},
        {"preproc", "keyword.directive"}, {"field", "property"},        {"parameter", "variable.parameter"},
        {"method", "function.method"},    {"namespace", "module"},      {"text.title", "markup.heading"},
        {"text.literal", "markup.raw"},   {"text.uri", "markup.link"},  {"text.reference", "markup.link"},
        {"text.strong", "markup.strong"}, {"text.emphasis", "markup.italic"},
    };
    for (const auto& n : other_names) {
        if (asked == n.name || (asked.size() > n.name.size() && asked.starts_with(n.name) && asked[n.name.size()] == '.'))
            return syntax_style(n.same_as);
    }
    return out;
}

void ThemeColors::set_syntax(const Entry& e) {
    for (Entry& have : syntax) {
        if (have.capture != e.capture) continue;
        if (e.has_color) have.color = e.color, have.has_color = true;
        if (e.italic >= 0) have.italic = e.italic;
        if (e.bold >= 0) have.bold = e.bold;
        if (e.underline >= 0) have.underline = e.underline;
        if (e.strike >= 0) have.strike = e.strike;
        return;
    }
    syntax.push_back(e);
}

void ThemeColors::set_syntax(std::string_view capture, Rgb c) {
    Entry e;
    e.capture = capture;
    e.color = c;
    e.has_color = true;
    set_syntax(e);
}

u32 Config::font_styles() const {
    u32 mask = 0;
    for (const bool dark : {true, false}) {
        const ThemeColors& t = theme(dark);
        for (const ThemeColors::Entry& e : t.syntax) mask |= 1u << font_style_of(t.syntax_style(e.capture));
    }
    return mask & ~1u;
}

const ThemeColors& Config::theme(bool dark) const {
    auto it = themes.find(dark ? theme_dark : theme_light);
    if (it == themes.end()) it = themes.find(dark ? "blit-dark" : "blit-light");
    return it->second;
}

std::string default_init_path() {
    if (const char* p = std::getenv("BLIT_INIT"); p && *p) return p;
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) return std::string(a) + "\\blit\\init.lua";
    return "init.lua";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/blit/init.lua";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/blit/init.lua";
    return "init.lua";
#endif
}

Config make_config(std::span<const ConfigSetting> settings) {
    Config c;
    add_builtin_themes(c);
    // All of them as one: the token styles for every theme go in before
    // each theme's own, so that a theme's own wins wherever it was set.
    Parser p{c, "init.lua", {}, {}};
    for (const ConfigSetting& s : settings) {
        p.from = s.from.empty() ? std::string("init.lua") : s.from;
        p.set(s.key, s.value);
    }
    p.finish();
    if (!c.themes.contains(c.theme_dark)) c.warnings += "unknown theme " + c.theme_dark + "\n";
    if (c.theme_light != c.theme_dark && !c.themes.contains(c.theme_light)) c.warnings += "unknown theme " + c.theme_light + "\n";
    return c;
}

std::string config_set(Config& c, const ConfigSetting& s) {
    const size_t had = c.warnings.size();
    Parser p{c, s.from.empty() ? std::string("init.lua") : s.from, {}, {}};
    p.set(s.key, s.value);
    p.finish();
    if (!c.themes.contains(c.theme_dark)) c.warnings += "unknown theme " + c.theme_dark + "\n";
    else if (!c.themes.contains(c.theme_light)) c.warnings += "unknown theme " + c.theme_light + "\n";
    std::string said = c.warnings.substr(had);
    c.warnings.resize(had);
    return said;
}

std::string toml_quote(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        if (ch == '\\' || ch == '"') out += '\\', out += ch;
        else if (ch == '\n') out += "\\n";
        else if (ch == '\t') out += "\\t";
        else out += ch;
    }
    return out + '"';
}

namespace {

// The settings that are one value, each with how to read it. (What sets
// them is Parser::set(); a test holds the two lists to one another.)
struct Getter {
    const char* key;
    ConfigValue (*get)(const Config&);
};
ConfigValue flag(bool b) {
    ConfigValue v;
    v.kind = ConfigValue::kBool, v.flag = b;
    return v;
}
ConfigValue num(double n) {
    ConfigValue v;
    v.kind = ConfigValue::kNumber, v.number = n;
    return v;
}
ConfigValue str(const std::string& s) {
    ConfigValue v;
    v.kind = ConfigValue::kText, v.text = s;
    return v;
}
ConfigValue list(const std::vector<std::string>& l) {
    ConfigValue v;
    v.kind = ConfigValue::kList, v.list = l;
    return v;
}
#define BLIT_GET(key, expr) {key, [](const Config& c) { return expr; }}
const Getter kGetters[] = {
    BLIT_GET("font.family", str(c.font_family)),
    BLIT_GET("font.style", str(c.font_style)),
    BLIT_GET("font.size", num(c.font_size)),
    BLIT_GET("font.line_height", num(c.line_height)),
    BLIT_GET("font.ligatures", flag(c.ligatures)),
    BLIT_GET("font.features", list(c.font_features)),
    BLIT_GET("theme.dark", str(c.theme_dark)),
    BLIT_GET("theme.light", str(c.theme_light)),
    BLIT_GET("editor.tab_width", num(c.tab_width)),
    BLIT_GET("editor.indent_width", num(c.indent_width)),
    BLIT_GET("editor.scrolloff", num(c.scrolloff)),
    BLIT_GET("remote.agent_idle", num(c.agent_idle)),
    BLIT_GET("remote.agent_keep", num(c.agent_keep)),
    BLIT_GET("editor.line_numbers", flag(c.line_numbers)),
    BLIT_GET("editor.relative_numbers", flag(c.relative_numbers)),
    BLIT_GET("editor.indent_guides", flag(c.indent_guides)),
    BLIT_GET("editor.tab_bar", flag(c.tab_bar)),
    BLIT_GET("editor.status_bar", flag(c.status_bar)),
    BLIT_GET("editor.start_screen", flag(c.start_screen)),
    BLIT_GET("editor.breadcrumbs", flag(c.breadcrumbs)),
    BLIT_GET("editor.sticky_scroll", flag(c.sticky_scroll)),
    BLIT_GET("editor.close_pairs", flag(c.close_pairs)),
    BLIT_GET("editor.restore", flag(c.restore)),
    BLIT_GET("editor.local_history", flag(c.local_history)),
    BLIT_GET("editor.minimap", flag(c.minimap)),
    BLIT_GET("editor.spell_check", flag(c.spell)),
    BLIT_GET("editor.soft_wrap", flag(c.soft_wrap)),
    BLIT_GET("panel.watch", flag(c.watch)),
    BLIT_GET("updates.check", flag(c.update_check)),
    BLIT_GET("look.presentation_size", num(c.presentation_size)),
    BLIT_GET("editor.soft_wrap_languages", list(c.soft_wrap_languages)),
    BLIT_GET("editor.modal", flag(c.modal)),
    BLIT_GET("syntax.enabled", flag(c.syntax)),
    BLIT_GET("syntax.max_file_mb", num(c.syntax_max_mb)),
    BLIT_GET("syntax.parser_dirs", list(c.parser_dirs)),
    BLIT_GET("syntax.query_dirs", list(c.query_dirs)),
    BLIT_GET("syntax.install", list(c.syntax_install)),
    BLIT_GET("syntax.auto_install", flag(c.syntax_auto_install)),
    BLIT_GET("syntax.italic", flag(c.syntax_italic)),
    BLIT_GET("syntax.bold", flag(c.syntax_bold)),
    BLIT_GET("syntax.bracket_colors", flag(c.syntax_brackets)),
    BLIT_GET("lsp.enabled", flag(c.lsp)),
    BLIT_GET("lsp.max_file_mb", num(c.lsp_max_mb)),
    BLIT_GET("lsp.completion", flag(c.lsp_completion)),
    BLIT_GET("lsp.inline_diagnostics", flag(c.lsp_inline)),
    BLIT_GET("lsp.hover_ms", num(c.lsp_hover_ms)),
    BLIT_GET("lsp.format_on_save", flag(c.lsp_format_on_save)),
    BLIT_GET("lsp.inlay_hints", flag(c.lsp_inlay_hints)),
    BLIT_GET("lsp.semantic_colors", flag(c.lsp_semantic)),
    BLIT_GET("lsp.highlight_uses", flag(c.lsp_uses)),
    BLIT_GET("lsp.reference_counts", flag(c.lsp_ref_counts)),
    BLIT_GET("git.enabled", flag(c.git)),
    BLIT_GET("build.notify", flag(c.build_notify)),
    BLIT_GET("serial.baud", num(c.serial_baud)),
    BLIT_GET("serial.line_end", str(c.serial_line_end)),
    BLIT_GET("rtt.line_end", str(c.rtt_line_end)),
    BLIT_GET("timeline.max_mb", num(c.timeline_max_mb)),
    BLIT_GET("wave.remote_fetch_mb", num(c.wave_remote_fetch_mb)),
    BLIT_GET("debug.inline_values", flag(c.debug_inline_values)),
    BLIT_GET("python.interpreter", str(c.python_interpreter)),
    BLIT_GET("panel.open", flag(c.panel_open)),
    BLIT_GET("panel.width", num(c.panel_width)),
    BLIT_GET("scroll.lines", num(c.scroll_lines)),
    BLIT_GET("scroll.ease_ms", num(c.scroll_ease_ms)),
    BLIT_GET("scroll.momentum", flag(c.scroll_momentum)),
    BLIT_GET("search.ignore", list(c.search_ignore)),
    BLIT_GET("search.gitignore", flag(c.search_gitignore)),
    BLIT_GET("search.hidden", flag(c.search_hidden)),
    BLIT_GET("markdown.web_pictures", flag(c.markdown_web_pictures)),
    BLIT_GET("look.rounding", num(c.look_rounding)),
    BLIT_GET("look.shadows", flag(c.look_shadows)),
    BLIT_GET("look.animations", flag(c.look_animations)),
    BLIT_GET("look.cursor_glide_ms", num(c.look_cursor_glide_ms)),
    BLIT_GET("look.dim_inactive", num(c.look_dim_inactive)),
    BLIT_GET("window.width", num(c.window_width)),
    BLIT_GET("window.height", num(c.window_height)),
    BLIT_GET("keys.timeout_ms", num(c.key_timeout_ms)),
    BLIT_GET("keys.leader", str(c.key_leader)),
    BLIT_GET("script.timeout_ms", num(c.script_timeout_ms)),
};
#undef BLIT_GET

}  // namespace

ConfigValue config_get(const Config& c, std::string_view key) {
    for (const Getter& g : kGetters)
        if (key == g.key) return g.get(c);
    return {};
}

std::string config_lua(const ConfigValue& v) {
    auto quote = [](std::string_view s) {
        std::string q = "\"";
        for (const char ch : s) {
            if (ch == '"' || ch == '\\') q += '\\', q += ch;
            else if (ch == '\n') q += "\\n";
            else if (static_cast<unsigned char>(ch) < 0x20) {  // (three digits: a digit after it is not taken into it)
                char e[8];
                std::snprintf(e, sizeof e, "\\%03u", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                q += e;
            }
            else q += ch;
        }
        return q + "\"";
    };
    switch (v.kind) {
        case ConfigValue::kBool: return v.flag ? "true" : "false";
        case ConfigValue::kNumber: {
            char b[32];
            std::snprintf(b, sizeof b, "%g", v.number);
            return b;
        }
        case ConfigValue::kText: return quote(v.text);
        case ConfigValue::kList: {
            std::string out = "{";
            for (size_t i = 0; i < v.list.size(); ++i) out += (i ? ", " : " ") + quote(v.list[i]);
            return out + (v.list.empty() ? "}" : " }");
        }
        default: return {};
    }
}

std::span<const char* const> config_keys() {
    static const std::vector<const char*> keys = [] {
        std::vector<const char*> k;
        for (const Getter& g : kGetters) k.push_back(g.key);
        return k;
    }();
    return keys;
}

}  // namespace blit
