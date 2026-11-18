#pragma once
// Settings: the defaults, and what init.lua sets with blit.opt (see
// init.example.lua for every one). A setting that is wrong becomes a
// warning and the default stays in place, so a typo never stops the editor
// from opening.
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct Rgb {
    float r = 0, g = 0, b = 0;
};

// How one kind of token is drawn.
struct TokenStyle {
    Rgb color;
    bool italic = false, bold = false;
    bool underline = false, strike = false;
};

struct ThemeColors {
    Rgb background, foreground, selection, cursor, scrollbar;
    // Language server display: the four kinds of diagnostic, quiet and
    // emphasized text in popups, and the popups themselves.
    Rgb error, warning, info, hint, muted, accent, popup, popup_border;
    // The lines that mark levels of indentation: the first level's colour,
    // the second's, and so on, starting over after the last (at most six).
    std::vector<Rgb> guides;
    // Brackets by how deep they are: the first pair's colour, the second's,
    // round and round (none: they are punctuation like any other).
    std::vector<Rgb> brackets;

    // What the theme says about one tree-sitter capture name ("keyword",
    // "string.escape"). An entry sets only some of colour, italic and bold;
    // the rest comes from the capture's parents.
    struct Entry {
        std::string capture;
        Rgb color;
        bool has_color = false;
        std::int8_t italic = -1, bold = -1;  // 1 on, 0 off, -1 not said here
        std::int8_t underline = -1, strike = -1;
    };
    std::vector<Entry> syntax;

    // The style of a capture. Each of colour, italic and bold comes from the
    // nearest of: its own entry, then its parents' ("a.b.c" looks at "a.b",
    // then "a"). With no entry at all it is plain foreground text.
    TokenStyle syntax_style(std::string_view capture) const;
    Rgb syntax_color(std::string_view capture) const { return syntax_style(capture).color; }
    // Adds what `e` sets to the capture's entry, leaving the rest of it alone.
    void set_syntax(const Entry& e);
    void set_syntax(std::string_view capture, Rgb c);
};

// Where the tree-sitter grammar for a language comes from: a repository on
// GitHub at a fixed revision. In the settings file, a grammar with the name
// of a built-in one starts from that and changes what it lists.
struct GrammarSource {
    std::string name;                   // blit's name for the language: "cpp"
    std::string repo;                   // "owner/name" on GitHub
    std::string rev;                    // the commit (or tag) to fetch
    std::string path;                   // folder in the repository that holds src/parser.c; "" = the top
    std::vector<std::string> inherits;  // languages whose highlight queries come before its own
    std::vector<std::string> extensions;  // file endings that are this language: "rs" (settings only)
    bool operator==(const GrammarSource&) const = default;
};

// How to run one language server. In the settings file, a server with the
// name of a built-in one starts from that and changes what it lists.
struct LspServer {
    std::string name;
    std::vector<std::string> command;    // program and arguments
    std::vector<std::string> languages;  // blit's language names: "cpp", "python", ...
    std::vector<std::string> roots;      // files that mark the top folder of a project
    std::string init_options;            // JSON for the server's initializationOptions, or empty
    std::string settings;                // JSON the server may ask for, or empty
    bool enabled = true;                 // false: never start this one
    bool operator==(const LspServer&) const = default;
};

struct Config {
    // [font]
    std::string font_family;  // family name or file path; empty = a system monospace font
    std::string font_style = "Regular";
    float font_size = 13;      // points
    float line_height = 1.0f;  // multiplies the font's own line spacing
    bool ligatures = false;
    std::vector<std::string> font_features;  // OpenType features: "ss01", "zero", "-calt"

    // [theme]
    std::string theme_dark = "blit-dark";    // used when the system is in dark mode
    std::string theme_light = "blit-light";  // used when it is in light mode
    std::map<std::string, ThemeColors, std::less<>> themes;  // built in, plus [themes.NAME]

    // [editor]
    u32 tab_width = 4;
    bool line_numbers = true;   // a column of line numbers left of the text
    // Vim's relativenumber with number: the cursor's line shows its own
    // number, every other line how far it is from the cursor (what to put
    // before j or k to get there). Only with Vim's modes on.
    bool relative_numbers = true;
    bool indent_guides = true;  // a coloured line down each level of indentation
    u32 indent_width = 0;       // columns per level for those lines; 0 = worked out from each file
    bool tab_bar = true;        // the row of open files along the top
    bool status_bar = true;     // the row across the bottom: the repository, the file, the cursor, problems
    bool breadcrumbs = true;
    bool sticky_scroll = true;
    bool close_pairs = true;    // an opening bracket or quote typed puts in its closer (see App::pair_text)  // the lines that open the scopes the top of the view is in, pinned there    // under each split's tabs: the file's folders and name, and the definitions the cursor is in
    bool start_screen = true;   // started with no file: the start screen (what to do first, what was opened lately)
    bool restore = true;        // a project folder opened again: its tabs, splits, cursors and side panel as they were left
    bool modal = true;          // Vim's modes and keys; false: an editor where typing types

    // [syntax]
    bool syntax = true;         // highlight with tree-sitter when a grammar is installed
    float syntax_max_mb = 32;   // files larger than this stay plain
    std::vector<std::string> parser_dirs;  // extra folders with <language>.so grammars
    std::vector<std::string> query_dirs;   // extra folders with <language>/highlights.scm
    std::vector<std::string> syntax_install;  // grammars blit fetches and builds if they are not installed
    bool syntax_auto_install = false;         // also the grammar of any file that is opened
    std::vector<GrammarSource> grammars;      // [syntax.grammars.NAME]
    bool syntax_italic = true;  // false: draw nothing in italics, whatever the theme says
    bool syntax_bold = true;    // same for bold
    bool syntax_brackets = true;  // brackets coloured by how deep they are (the theme's `brackets`, round and round)

    // [lsp]
    bool lsp = true;                 // start a language server when one is installed for the file type
    float lsp_max_mb = 8;            // files larger than this get none
    bool lsp_completion = true;      // offer completions while typing (Ctrl+Space always does)
    bool lsp_inline = true;          // show the message of a problem at the end of the cursor's line
    float lsp_hover_ms = 350;        // how long the mouse rests before a hover popup
    bool lsp_format_on_save = false; // the server formats a file before it is written
    bool lsp_inlay_hints = false;    // parameter names and deduced types between the characters, as the server gives them
    bool lsp_semantic = true;        // the server's colours (semantic tokens) over tree-sitter's, where it says more
    bool lsp_uses = true;            // the other uses of the symbol at the cursor marked, once it has rested
    bool minimap = true;             // the file's lines small down the right of a wide enough split (appminimap.cpp)
    bool local_history = true;       // each file's text kept at its saves and while it is typed in (apphistory.cpp)
    bool lsp_ref_counts = true;      // how many references each definition on show has, after its line
    std::vector<LspServer> lsp_servers;  // [lsp.servers.NAME], tried before the built-in ones

    // [git]
    bool debug_inline_values = true;  // while stopped, the values of the variables a line names after its end
    bool git = true;        // show git's view of the files: changed lines, the Git tab of the side panel

    // [panel]
    bool panel_open = false;  // show the side panel when blit starts
    int panel_width = 32;     // its width, in characters

    // [scroll]
    float scroll_lines = 3;     // lines per mouse wheel notch
    float scroll_ease_ms = 45;  // how quickly a wheel scroll settles; 0 jumps
    bool scroll_momentum = true;  // a touchpad flick keeps gliding and slows to a stop

    // [search]
    std::vector<std::string> search_ignore;  // more to leave out of searches, in .gitignore's syntax
    bool search_gitignore = true;            // leave out what .gitignore files say
    bool search_hidden = true;               // show files and folders whose names start with a dot

    // [markdown]
    bool markdown_web_pictures = false;  // the preview fetches pictures on the web (else their alt text shows, as a link)

    // [look]
    float look_rounding = 6;          // corner radius, in points, that everything rounded is sized from; 0: square corners
    bool look_shadows = true;         // popups cast a shadow, and so do the tab bar and line numbers over text that has slid under them
    bool look_animations = true;      // popups fade in; the scroll thumb fades away when left alone
    float look_cursor_glide_ms = 60;  // how long the cursor takes from one place to the next; 0: it jumps
    float look_dim_inactive = 0;      // how far a split without the keyboard is dimmed toward the background (0 to 0.6)

    // [window]
    int window_width = 1100, window_height = 760;

    // [keys] and [script]: for init.lua and the plugins (applua.cpp).
    float key_timeout_ms = 1000;      // how long the first keys of a mapping wait for the rest (Vim's 'timeoutlen')
    std::string key_leader = "\\";    // what <leader> in a mapping stands for
    float script_timeout_ms = 3000;   // a script that runs longer than this in one go is stopped

    std::string warnings;  // one per line; empty when every setting was right

    const ThemeColors& theme(bool dark) const;
    // The font style a token style asks for: Font::kItalic, kBold,
    // kUnderline and kStrike or-ed together, after [syntax] italic/bold had
    // their say.
    u32 font_style_of(const TokenStyle& t) const {
        return (t.italic && syntax_italic ? 1u : 0u) | (t.bold && syntax_bold ? 2u : 0u) | (t.underline ? 4u : 0u) | (t.strike ? 8u : 0u);
    }
    // Every font style the two chosen themes can ask for, one bit each
    // (bit 1 italic, bit 2 bold, bit 3 bold italic).
    u32 font_styles() const;
};


// One setting: its key ("font.size"), its value as a literal ("14",
// "\"Victor Mono\"", "true", "[\"ss01\", \"zero\"]"; the script side makes
// these from Lua values), and where it was made, which goes in front of a
// warning about it ("init.lua:12").
struct ConfigSetting {
    std::string key, value, from;
};

// $BLIT_INIT, else $XDG_CONFIG_HOME/blit/init.lua, else
// ~/.config/blit/init.lua (%APPDATA%\blit\init.lua on Windows).
std::string default_init_path();

// The defaults with these settings on top, in order (the token styles of
// syntax.styles before each theme's own, so that a theme's own wins).
Config make_config(std::span<const ConfigSetting> settings = {});

// What a setting is at present, for the settings that are one value (a
// switch, a number, a string, a list of strings). kNone: there is no such
// setting, or it is made of several (a theme, a language server).
struct ConfigValue {
    enum : u8 { kNone, kBool, kNumber, kText, kList };
    u8 kind = kNone;
    bool flag = false;
    double number = 0;
    std::string text;
    std::vector<std::string> list;
};
ConfigValue config_get(const Config& c, std::string_view key);
// Puts one setting on top of what c is. Returns what is wrong with it
// (empty if nothing is), and then leaves c as it was in that respect.
std::string config_set(Config& c, const ConfigSetting& s);
// Every key config_get() answers for.
std::span<const char* const> config_keys();
// A setting's value as init.lua writes it: true, 13, "Menlo", { "a", "b" }.
std::string config_lua(const ConfigValue& v);
// A string as a setting's literal value, quotes and all.
std::string toml_quote(std::string_view s);

// "#rgb" or "#rrggbb".
bool parse_color(std::string_view s, Rgb* out);
// A token style as the config file writes it: a colour, "italic", "bold" and
// "regular" (neither italic nor bold) in any combination, separated by
// spaces: "#908caa italic", "bold", "regular". Sets only what is named.
bool parse_token_style(std::string_view s, ThemeColors::Entry* out);

}  // namespace blit
