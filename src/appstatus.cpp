// The status bar: a row across the bottom of the window, under the splits
// and the side panel. On the left Vim's mode (with modal editing), the
// repository (the branch, how far it is ahead of and behind the one it
// follows, the lines of this file changed since the last commit, how many
// files are changed) and the file (its path in the project folder, unsaved
// or read only); on the right what a script put there (blit.ui.status), the
// language server's problems, the cursor's line and column, the
// indentation, the line endings, the language and its server ("(no
// colours)" when the language's grammar is not installed). A click on the
// repository opens the Git tab of the side panel, one on the problems the
// Problems tab; one on the line and column opens Go to line, and one on the
// indentation, the line endings or the language a small list that changes
// it for the file. On the start screen it names the project folder and the
// settings file instead.
//
// Its cells are made again only when something it shows has changed; a
// frame that changes nothing in it costs a comparison of a few numbers.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <cstdio>
#include <string>
#include <vector>

#include "app.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

enum Action : u8 { kNone, kGitTab, kProblemsTab, kReconnect, kSettings, kGoLine, kIndent, kLineEnds, kLanguage };

// The bar's tones (Popup::tones), behind Vim's mode: the muted ink's in
// Normal and Command mode, the accent's in the modes that change the text.
enum : u32 { kToneMuted = 1, kToneAccent = 2 };

// One part of the bar: its text, its ink, what a click on it does, how many
// blank columns come before it (after the first), and (on the right) how
// soon it goes when there is not room for all: the lowest first.
struct Seg {
    std::string text;
    Ink in = ink::kMuted;
    Action action = kNone;
    u32 gap = 2;
    int keep = 0;
    bool shrink = false;  // a path's folders: shortened from the start before anything goes
    u32 tone = 0;         // a lit run behind it, in this of the bar's tones (0: none)
    u32 least = 0;        // the columns it takes at the least (the mode's: the longest name's, so that what follows stays put)
    u32 icon = 0;         // the bytes of its text that are an icon and its space (the font's, where it has one)
};

}  // namespace

// A language's name as people write it ("C++" for "cpp"): the name its
// grammar gives it (in the settings, or built in), or one from this list;
// one not listed with a capital.
std::string language_name(const std::string& id, std::span<const GrammarSource> own) {
    for (const std::span<const GrammarSource> list : {own, std::span<const GrammarSource>(builtin_grammars())})
        for (const GrammarSource& g : list)
            if (g.name == id && !g.display.empty()) return g.display;
    static const struct {
        const char* id;
        const char* name;
    } names[] = {{"cpp", "C++"},   {"c", "C"},         {"json", "JSON"},   {"toml", "TOML"},
                 {"yaml", "YAML"}, {"bash", "Shell"},  {"cmake", "CMake"}, {"make", "Makefile"}};
    for (const auto& n : names)
        if (id == n.id) return n.name;
    std::string out = id;
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 32);
    for (char& c : out)
        if (c == '_') c = ' ';
    return out;
}

namespace {

std::string count_text(u64 n) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(n));
    return b;
}

// Vim's mode as the bar names it.
const char* mode_name(Vim::Mode m) {
    switch (m) {
        case Vim::kInsert: return "INSERT";
        case Vim::kReplace: return "REPLACE";
        case Vim::kVisual: return "VISUAL";
        case Vim::kVisualLine: return "V-LINE";
        case Vim::kVisualBlock: return "V-BLOCK";
        case Vim::kCommand: return "COMMAND";
        default: return "NORMAL";
    }
}
constexpr u32 kModeCols = 7;  // the longest of those names

// A path on this machine with the home folder written "~".
void home_short(std::string& path) {
    const char* home = SDL_getenv("HOME");
    if (!home || !*home) return;
    const std::string_view h(home);
    if (path == h) path = "~";
    else if (path.size() > h.size() + 1 && path.starts_with(h) && path[h.size()] == '/') path.replace(0, h.size(), "~");
}

}  // namespace

struct App::StatusUi {
    u64 key = 0;
    TextBox box;
    std::vector<Seg> left, right;
    // Where each part landed, for clicks: [c0, c1) and what it does.
    struct Spot {
        u32 c0, c1;
        Action action;
        std::string_view text;  // (its part's, in left or right: they stay as they are until the spots are made again)
    };
    std::vector<Spot> spots;
    // Where the cursor is: what it was made from, its part in `right`, and
    // the columns it took.
    u64 at_key = 0;
    size_t at_seg = ~size_t{0};
    u32 at_c0 = 0, at_w = 0;
    // The language of a file, kept for it (it looks at the file's name and
    // first line), and whether its grammar is missing (that looks at the disk).
    u64 lang_doc = ~0ull, colours_key = ~0ull;
    std::string lang_path, lang, lang_chosen;
    bool no_colours = false;
    // The project folder a file is shown in while the side panel has none:
    // the one the panel would open at, kept for the file (finding it looks
    // at the disk).
    u64 base_doc = ~0ull;
    std::string base_path, base;
    // The language server's problems in the file, counted once for each set
    // of them (and of the build's, which may have found some of them too).
    u64 diag_gen = ~0ull, diag_doc = ~0ull, diag_build = ~0ull;
    size_t errors = 0, warnings = 0;
    float tones[8][4] = {};  // (Popup::tones)
};

int App::status_px() const {
    if (!cfg_.status_bar || !font_ || focus_mode_) return 0;
    return font_->cell_h() + font_->cell_h() / 4;
}

void App::status_layout(int w, int h, FrameDesc& fd) {
    const int tall = status_px();
    if (!tall || fd.popup_count >= std::size(fd.popups)) return;
    if (!status_) status_ = std::make_shared<StatusUi>();
    StatusUi& u = *status_;
    Font& font = *font_;
    const Look l = look();
    const int cw = font.cell_w(), ch = font.cell_h(), pad_x = l.edge;
    const u32 cols = static_cast<u32>(std::max(1, (w - 2 * pad_x) / cw));
    Doc& d = *pane_->doc;
    const Editor& ed = d.ed;
    // (A terminal's tab, a profile's, a picture's and a PDF's have no lines
    // and columns of their own, and nothing for Vim's modes to do.)
    const bool text = !d.term && !d.prof && !d.image && !d.pdf;
    const bool start = start_shown();  // the start screen, or its New project tab: the project folder and the settings file
    const bool modal = cfg_.modal && text && !start;
    const bool round = l.pill > 0;
    const std::string* root = panel_root();

    // Everything it shows, in a few numbers: when they are as they were, so are the cells.
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    const u64 path_hash = std::hash<std::string>{}(ed.path);
    mix(d.id);
    mix(ed.modified() ? 1 : 0);
    mix(d.read_only ? 1 : 0);
    mix(d.loading ? 1 : 0);
    mix(remote_gen_);
    mix(path_hash);
    // (Whether the colours are missing: what that depends on, as one number. The disk is looked at only when it changes.)
    const u64 colours = ((((d.id * 1000003 + path_hash) * 1000003 + grammars_gen_) * 4 + (d.syntax.active() ? 2 : 0) + (cfg_.syntax ? 1 : 0)) * 1000003) ^
                        std::hash<std::string>{}(d.lang);
    mix(colours);
    mix(ed.crlf() ? 1 : 0);
    mix(git_gen_);
    mix(d.hunks_gen);
    mix(lsp_.diagnostics_generation() * 1000003 + lsp_.state_generation());
    mix(build_prob_gen_);
    mix(std::hash<std::string>{}(script_status_));
    mix(d.indent_tabs ? indent_step() + 1000 : indent_step());
    mix(cols);
    mix(font.generation());
    mix(start ? 1 : 0);
    mix(modal ? vim_.mode() + 1u : 0);
    mix(round ? 1 : 0);
    mix(root ? std::hash<std::string>{}(*root) : 0);
    bool kernel_lost = false;
    const std::string kernel = start ? std::string() : nb_kernel_status(d, &kernel_lost);  // (a notebook's)
    mix(std::hash<std::string>{}(kernel));
    // And apart from those, where the cursor is and what is selected (the
    // text's revision for what that holds): when only these change, as with
    // every key typed, and the words for them are as wide as they were, they
    // are written over their old place and nothing else is made again.
    u64 at_key = 1469598103934665603ull;
    auto mix_at = [&](u64 v) { at_key = (at_key ^ v) * 1099511628211ull; };
    mix_at(ed.revision());
    mix_at(pane_->cur_line);
    mix_at(pane_->cur_col);
    mix_at(ed.anchor() ^ (ed.cursor() << 1));
    u64 vlo = 0, vhi = 0;
    const bool visual = vim_selection(&vlo, &vhi);
    mix_at(visual ? vlo * 31 + vhi : 0);
    auto at_text = [&] {
        std::string at = "Ln " + count_text(pane_->cur_line + 1) + ", Col " + count_text(static_cast<u64>(pane_->cur_col) + 1);
        u64 lo = visual ? vlo : std::min(ed.anchor(), ed.cursor()), hi = visual ? vhi : std::max(ed.anchor(), ed.cursor());
        if (ed.multi()) {  // (more carets than one: how many, for what one has selected says little)
            at += " (" + count_text(ed.carets().size() + 1) + " carets)";
        } else if (hi > lo) {
            const u64 l0 = ed.buf.line_of(lo), l1 = ed.buf.line_of(hi);
            if (l1 > l0) {
                at += " (" + count_text(l1 - l0 + 1) + " lines)";
            } else if (hi - lo <= 65536) {  // (characters, counted; a longer stretch is told in bytes)
                const std::string t = ed.buf.text(lo, hi - lo);
                u64 chars = 0;
                for (const char c : t) chars += (static_cast<u8>(c) & 0xC0) != 0x80;
                at += " (" + count_text(chars) + " selected)";
            } else {
                at += " (" + count_text((hi - lo) >> 10) + " KB selected)";
            }
        }
        return at;
    };
    if (key == u.key && at_key != u.at_key && u.at_seg < u.right.size()) {
        std::string at = at_text();
        if (width_of(at) == u.at_w) {
            put(u.box, font, 0, u.at_c0, at, ink::kMuted);
            Seg& s = u.right[u.at_seg];
            s.text = std::move(at);
            for (StatusUi::Spot& spot : u.spots)
                if (spot.action == kGoLine) spot.text = s.text;
            u.at_key = at_key;
        }
    }
    if (key != u.key || at_key != u.at_key) {
        u.key = key;
        u.at_key = at_key;
        u.left.clear();
        u.right.clear();
        // A path as its folders (muted, and shortened from their start first
        // when there is not room) and its name.
        auto add_path = [&](const std::string& path, u32 gap) {
            const size_t slash = path.rfind('/');
            if (slash == std::string::npos || slash + 1 == path.size()) {
                u.left.push_back({path, ink::kStrong, kNone, gap});
                return;
            }
            u.left.push_back({path.substr(0, slash + 1), ink::kMuted, kNone, gap, 0, true});
            u.left.push_back({path.substr(slash + 1), ink::kStrong, kNone, 0});
        };

        if (start) {
            // The start screen has no file: the folder it opens things in,
            // and the settings file (a click opens it).
            std::string folder = root ? *root : default_workdir();
            home_short(folder);
            add_path(folder, 2);
            std::string settings = init_path_;
            home_short(settings);
            u.right.push_back({std::move(settings), ink::kMuted, kSettings, 2, 1});
        } else {
            const GitSummary g = git_summary();  // (git_gen_ above says when this changes)

            // Vim's mode, first, on a lit run of its own.
            if (modal) {
                const Vim::Mode m = vim_.mode();
                const bool resting = m == Vim::kNormal || m == Vim::kCommand;
                Seg s{mode_name(m), resting ? ink::kHeading : ink::kAccentStrong};
                s.tone = resting ? kToneMuted : kToneAccent;
                s.least = kModeCols + (round ? 1 : 0);
                u.left.push_back(std::move(s));
            }

            // The machine, when the tab in front is on another one (a file's
            // own, a terminal's; for one with neither, the folder's, or the
            // one a file or folder opened could not be reached on), and how
            // its connection is.
            {
                const std::string& own = d.term ? term_folder(*d.term) : ed.path;
                std::string host, there;
                if (split_remote(own, &host, &there) || (own.empty() && ((root && split_remote(*root, &host, &there)) || !(host = remote_unreached()).empty()))) {
                    std::shared_ptr<Remote> r;
                    for (const std::shared_ptr<Remote>& c : Remote::all())
                        if (c->host() == host) r = c;
                    const Remote::State st = r ? r->state() : Remote::kConnecting;
                    const char* icon = font.has(0x21C4) ? "\xE2\x87\x84 " : "";
                    Seg s;
                    s.action = kNone;
                    s.icon = static_cast<u32>(std::strlen(icon));
                    if (st == Remote::kReady) {
                        s.text = icon + host;
                        s.in = ink::kText;
                    } else if (st == Remote::kFailed && r->retrying()) {  // (made again by itself: a click tries now)
                        s.text = std::string(icon) + "reconnecting to " + host + "\xE2\x80\xA6";
                        s.in = ink::kMuted;
                        s.action = kReconnect;
                    } else if (st == Remote::kFailed) {
                        s.text = icon + host + (r->stopped() ? ": agent stopped (connect again)" : ": not connected (connect again)");
                        s.in = ink::kError;
                        s.action = kReconnect;
                    } else {
                        const std::string doing = r ? r->progress() : std::string();
                        s.text = icon + (doing.empty() ? "connecting to " + host : doing) + "\xE2\x80\xA6";
                        s.in = ink::kMuted;
                    }
                    u.left.push_back(std::move(s));
                }
            }

            // The repository: the branch, and how it stands with the one it follows.
            if (g.repo) {
                const char* branch_icon = font.has(0xE0A0) ? "\xEE\x82\xA0 " : font.has(0x2387) ? "\xE2\x8E\x87 " : "";
                u.left.push_back({std::string(branch_icon) + (g.branch.empty() ? "(" + g.head + ")" : g.branch), ink::kText, kGitTab});
                u.left.back().icon = static_cast<u32>(std::strlen(branch_icon));
                std::string how;
                auto also = [&](const std::string& part) {
                    if (!how.empty()) how += ' ';
                    how += part;
                };
                if (g.ahead > 0) also((font.has(0x2191) ? "\xE2\x86\x91" : "+") + count_text(static_cast<u64>(g.ahead)));
                if (g.behind > 0) also((font.has(0x2193) ? "\xE2\x86\x93" : "-") + count_text(static_cast<u64>(g.behind)));
                if (g.merging) also("merging");
                if (!how.empty()) u.left.push_back({std::move(how), ink::kMuted, kGitTab, 1});
                // This file's lines that differ from the last commit.
                u64 added = 0, changed = 0, removed = 0;
                for (const GitHunk& k : d.hunks) {
                    if (k.kind == GitHunk::kAdded) added += k.count;
                    else if (k.kind == GitHunk::kModified) changed += k.count;
                    else removed += k.removed;
                }
                u32 gap = 2;  // (one between the counts)
                if (added) u.left.push_back({"+" + count_text(added), ink::kAdded, kGitTab, std::exchange(gap, 1u)});
                if (changed) u.left.push_back({"~" + count_text(changed), ink::kChanged, kGitTab, std::exchange(gap, 1u)});
                if (removed) u.left.push_back({(font.has(0x2212) ? "\xE2\x88\x92" : "-") + count_text(removed), ink::kRemoved, kGitTab, std::exchange(gap, 1u)});
                if (g.changed) u.left.push_back({count_text(g.changed) + (g.changed == 1 ? " file changed" : " files changed"), ink::kMuted, kGitTab});
            }
            // The file: its path in the project folder when it is in it, else
            // with ~ for the home folder. The project folder is the side
            // panel's, or, before it has one, the one it would open at for
            // this file (the same whether git has answered or not); a file on
            // another machine has none until a folder there is opened (finding
            // one would ask that machine).
            {
                std::string path = ed.path.empty() ? (d.title.empty() ? std::string("untitled") : d.title) : full_path(ed.path);
                const std::string* base = root;
                if (!base && !ed.path.empty() && !is_remote(path)) {
                    if (u.base_doc != d.id || u.base_path != ed.path) {
                        u.base_doc = d.id;
                        u.base_path = ed.path;
                        u.base = default_workdir();
                    }
                    base = &u.base;
                }
                std::string host, there;
                if (!ed.path.empty() && base && !base->empty() && path.size() > base->size() + 1 && path.starts_with(*base) && path[base->size()] == '/') {
                    path.erase(0, base->size() + 1);
                } else if (split_remote(path, &host, &there)) {  // (on another machine, whose name is shown already: ~ for the home folder there)
                    path = there;
                    for (const std::shared_ptr<Remote>& c : Remote::all())
                        if (const std::string home_dir = c->host() == host ? c->home() : std::string();
                            !home_dir.empty() && path.size() > home_dir.size() + 1 && path.starts_with(home_dir) && path[home_dir.size()] == '/')
                            path = "~" + path.substr(home_dir.size());
                } else if (!ed.path.empty()) {
                    home_short(path);
                }
                add_path(path, g.repo ? 3 : 2);  // (a little more between the repository and the file)
                if (d.loading) u.left.push_back({"loading\xE2\x80\xA6", ink::kMuted, kNone, 2});
                else if (remote_saving(d.id)) u.left.push_back({"saving\xE2\x80\xA6", ink::kMuted, kNone, 2});  // (remote_gen_ says when this changes)
                else if (d.read_only && !d.term && !d.prof && !d.image && !d.pdf) u.left.push_back({"read only", ink::kMuted, kNone, 2});
                else if (ed.modified()) u.left.push_back({font.has(0x25CF) ? "\xE2\x97\x8F" : "*", ink::kAccent, kNone, 1});
            }

            // On the right, what is least needed first to go.
            if (!script_status_.empty()) u.right.push_back({script_status_, ink::kText, kNone, 2, 4});  // (the user's own: kept over the indentation and line ends)
            // The problems: the language server's in the open file, and the
            // last build's across files. One both found (in the file, on the
            // same line, as serious) is counted once.
            u32 errors = build_errors_, warnings = build_warnings_;
            bool server_ready = false;
            const std::string* server = lsp_.server_of(d.id, &server_ready);
            if (const std::vector<Diagnostic>* diags = server ? lsp_.diagnostics_of(d.id) : nullptr) {
                if (u.diag_gen != lsp_.diagnostics_generation() || u.diag_doc != d.id || u.diag_build != build_prob_gen_) {
                    u.diag_gen = lsp_.diagnostics_generation();
                    u.diag_doc = d.id;
                    u.diag_build = build_prob_gen_;
                    u.errors = u.warnings = 0;
                    const std::string file = is_remote(ed.path) ? ed.path : full_path(ed.path);
                    std::vector<std::pair<u32, u8>> built;  // (line from 1, severity)
                    for (const BuildProblem& p : build_problems())
                        if (p.path == file) built.emplace_back(p.line, p.severity);
                    std::ranges::sort(built);
                    std::vector<bool> taken(built.size());
                    for (const Diagnostic& x : *diags) {
                        if (x.severity != 1 && x.severity != 2) continue;
                        if (built.empty()) {  // (the usual case: nothing to match, so no line looked up)
                            ++(x.severity == 1 ? u.errors : u.warnings);
                            continue;
                        }
                        const std::pair<u32, u8> at{static_cast<u32>(ed.buf.line_of(std::min(x.start, ed.buf.size())) + 1), x.severity};
                        size_t i = static_cast<size_t>(std::ranges::lower_bound(built, at) - built.begin());
                        while (i < built.size() && built[i] == at && taken[i]) ++i;
                        if (i < built.size() && built[i] == at) {
                            taken[i] = true;
                            continue;
                        }
                        ++(x.severity == 1 ? u.errors : u.warnings);
                    }
                }
                errors += static_cast<u32>(u.errors), warnings += static_cast<u32>(u.warnings);
            }
            // (Nothing, while there is nothing to tell.)
            if (errors || warnings) {
                const char* err = font.has(0x2715) ? "\xE2\x9C\x95 " : "\xC3\x97 ";                                       // ✕, or ×
                const char* warn = font.has(0x26A0) ? "\xE2\x9A\xA0 " : font.has(0x25B2) ? "\xE2\x96\xB2 " : "! ";  // ⚠, or ▲
                u.right.push_back({err + count_text(errors), errors ? ink::kError : ink::kMuted, kProblemsTab, 2, 4});
                u.right.push_back({warn + count_text(warnings), warnings ? ink::kWarning : ink::kMuted, kProblemsTab, 1, 4});
            }
            if (!kernel.empty()) u.right.push_back({kernel, kernel_lost ? ink::kError : ink::kMuted, kNone, 2, 3});
            if (text) u.right.push_back({at_text(), ink::kMuted, kGoLine, 2, 5});
            const u32 step = text ? indent_step() : 0;
            if (text) u.right.push_back({d.indent_tabs ? std::string("Tabs") : "Spaces: " + count_text(step), ink::kMuted, kIndent, 2, 2});
            if (text) u.right.push_back({ed.crlf() ? "CRLF" : "LF", ink::kMuted, kLineEnds, 2, 2});
            if (u.lang_doc != d.id || u.lang_path != ed.path || u.lang_chosen != d.lang) {
                u.lang_doc = d.id;
                u.lang_path = ed.path;
                u.lang_chosen = d.lang;
                u.lang = file_language();
            }
            if (u.colours_key != colours) {
                u.colours_key = colours;
                u.no_colours = grammar_missing();
            }
            if (text) {
                u.right.push_back({u.lang.empty() ? std::string("Plain text") : language_name(u.lang, cfg_.grammars), ink::kMuted, kLanguage, 2, 3});
                // (What a narrow window drops first: the rest stays where it was without it.)
                if (u.no_colours) u.right.push_back({"(no colours)", ink::kMuted, kLanguage, 1, 1});
                if (server && !server->empty()) {  // (the tab's own: none for a tab on no server; what it is busy with, as it says)
                    const std::string* busy = server_ready ? lsp_.progress_of(d.id) : nullptr;
                    u.right.push_back({"\xC2\xB7 " + *server + (!server_ready ? " (starting)" : busy ? " (" + *busy + ")" : ""), ink::kMuted, kNone, 1, 2});  // (before the language)
                }
            }
        }

        // Laid out: the left from the start, the right against the end. What
        // does not fit goes: the right's least needed parts first (and what
        // of the left still does not fit is cut, with an ellipsis).
        auto width = [](const std::vector<Seg>& v) {
            u32 n = 0;
            for (size_t i = 0; i < v.size(); ++i) n += std::max(width_of(v[i].text), v[i].least) + (i ? v[i].gap : 0);
            return n;
        };
        auto over = [&] { return static_cast<i64>(width(u.left)) + static_cast<i64>(width(u.right)) + 3 - static_cast<i64>(cols); };
        // First the icons (a font's own pictures for a branch, another
        // machine): the words beside them say it without them.
        for (Seg& s : u.left)
            if (s.icon && over() > 0) s.text.erase(0, s.icon), s.icon = 0;
        // Then what is barely needed ("(no colours)": the rest stays as it would be without it).
        for (size_t i = u.right.size(); i-- > 0 && over() > 0;)
            if (u.right[i].keep <= 1) u.right.erase(u.right.begin() + static_cast<std::ptrdiff_t>(i));
        // Then the path's folders, from their start ("…/src/"), and if
        // that is not enough, all of them.
        for (size_t i = 0; i < u.left.size() && over() > 0; ++i) {
            if (!u.left[i].shrink) continue;
            Seg& f = u.left[i];
            const i64 cut = over() + 1;  // (and one column for the "…")
            size_t at = 0;
            for (i64 n = 0; n < cut && at < f.text.size(); ++n) at += decode_utf8(reinterpret_cast<const u8*>(f.text.data()) + at, f.text.size() - at).len;
            at = at > 0 ? f.text.find('/', at - 1) : 0;  // (whole folders go)
            if (at != std::string::npos && at + 1 < f.text.size()) {
                f.text = "\xE2\x80\xA6" + f.text.substr(at);
            } else {
                if (i + 1 < u.left.size()) u.left[i + 1].gap = f.gap;
                u.left.erase(u.left.begin() + static_cast<std::ptrdiff_t>(i));
            }
            break;
        }
        while (!u.right.empty() && over() > 0) {
            auto least = std::min_element(u.right.begin(), u.right.end(), [](const Seg& a, const Seg& b) { return a.keep < b.keep; });
            u.right.erase(least);
        }
        u.box.resize(cols, 1);
        u.spots.clear();
        u32 c = 0;
        for (size_t i = 0; i < u.left.size(); ++i) {
            const Seg& s = u.left[i];
            if (i > 0) c += s.gap;
            if (c >= cols) break;
            const u32 c0 = c, room = cols - c;
            c = width_of(s.text) <= room ? put(u.box, font, 0, c, s.text, s.in) : put(u.box, font, 0, c, fit_text(s.text, room, s.shrink), s.in);
            if (s.tone) {
                // A lit run, with a column after the text for its rounded end.
                const u32 end = std::min(cols, c + (round ? 1u : 0u));
                for (u32 k = c0; k < end; ++k) u.box.cells[k] |= kCellSelected | (s.tone << kCellDecoShift);
                c = end;
            }
            c = std::max(c, std::min(cols, c0 + s.least));
            if (s.action != kNone) u.spots.push_back({c0, c, s.action, s.text});
        }
        const u32 right_w = width(u.right);
        c = cols > right_w ? cols - right_w : 0;
        u.at_seg = ~size_t{0};
        for (size_t i = 0; i < u.right.size(); ++i) {
            const Seg& s = u.right[i];
            if (i > 0) c += s.gap;
            const u32 c0 = c;
            c = put(u.box, font, 0, c, s.text, s.in);
            if (s.action != kNone) u.spots.push_back({c0, c, s.action, s.text});
            if (s.action == kGoLine) u.at_seg = i, u.at_c0 = c0, u.at_w = c - c0;
        }
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.x = 0;
    p.y = h - tall;
    p.w = w;
    p.h = tall;
    p.pad_x = pad_x;
    p.pad_y = (tall - ch) / 2;
    p.cols = u.box.cols;
    p.rows = 1;
    p.cells = u.box.cells.data();
    p.border_px = 0;
    if (modal) {
        // The mode's tones: the bar's background washed with an ink's colour
        // (worked out each frame: the theme changes with the file in front).
        const float* bg = theme_.popup_bg;
        auto wash = [&](float* out, const float* in, float k) {
            for (int i = 0; i < 3; ++i) out[i] = bg[i] + (in[i] - bg[i]) * k;
            out[3] = 1;
        };
        wash(u.tones[kToneMuted], theme_.palette[kInkMuted], 0.18f);
        wash(u.tones[kToneAccent], theme_.palette[kInkAccent], 0.22f);
        p.tones = &u.tones[0][0];
        // Rounded, it reaches out into the margin to the inset, as a lit row does.
        if (round) {
            p.pill = l.pill;
            p.pill_out = static_cast<float>(std::max(0, pad_x - l.inset));
            p.pill_dy = std::round(scale_);
        }
    }
}

bool App::status_mouse_down(const SDL_MouseButtonEvent& b) {
    const int tall = status_px();
    if (!tall || !status_) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (py < px_h_ - tall) return false;
    if (b.button == SDL_BUTTON_LEFT) {
        const int cw = font_->cell_w(), pad_x = look().edge;
        const u32 col = px > pad_x ? static_cast<u32>((px - pad_x) / cw) : 0;
        for (const StatusUi::Spot& s : status_->spots)
            if (col >= s.c0 && col < s.c1) {
                if (s.action == kGitTab) panel_show_git();
                else if (s.action == kProblemsTab) panel_show_problems();
                else if (s.action == kReconnect) remote_reconnect();
                else if (s.action == kSettings) settings_open();
                else if (s.action == kGoLine) find_open(kFindNames, ":");  // (Go to file's ":120")
                else if (s.action == kIndent) status_pick(0);
                else if (s.action == kLineEnds) status_pick(1);
                else if (s.action == kLanguage) status_pick(2);
                break;
            }
    }
    dirty_ = true;
    return true;  // (the bar has the click, whatever it was on)
}

// A click on the middle of the part of the bar whose text starts with
// `part`, through the real event handler (for the tests).
bool App::status_click(const std::string& part) {
    if (!status_ || !status_px()) return false;
    for (const StatusUi::Spot& s : status_->spots) {
        if (!s.text.starts_with(part)) continue;
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.clicks = 1;
        e.button.x = static_cast<float>(look().edge + static_cast<int>((s.c0 + s.c1) * static_cast<u32>(font_->cell_w()) / 2)) / density_;
        e.button.y = static_cast<float>(px_h_ - status_px() / 2) / density_;
        handle(e);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the lists a click opens

namespace {

constexpr u32 kIndentSteps[] = {2, 3, 4, 8};  // (spaces; the first row is tabs)

}  // namespace

void App::status_pick(int what) {
    Doc& d = *pane_->doc;
    const u64 id = d.id;
    if (what == 0) {  // the indentation: tabs, or so many spaces
        std::vector<std::string> items{"Tabs"};
        size_t at = 0;
        const u32 step = indent_step();
        for (const u32 n : kIndentSteps) {
            if (!d.indent_tabs && n == step) at = items.size();
            items.push_back("Spaces: " + count_text(n));
        }
        find_preview_item("Indentation", std::move(items), at, nullptr, [this, id](int i) {
            Doc* doc = find_doc(id);
            if (!doc || i < 0) return;
            doc->indent_chosen = true;
            doc->indent_tabs = i == 0;
            doc->indent_step = i == 0 ? 0 : kIndentSteps[i - 1];
            if (pane_->doc == doc) vim_attach();  // (what Vim's > shifts by)
            dirty_ = true;
        }, nullptr);
    } else if (what == 1) {  // the line ends
        find_preview_item("Line endings", {"LF", "CRLF"}, d.ed.crlf() ? 1 : 0, nullptr, [this, id](int i) {
            if (Doc* doc = find_doc(id); doc && i >= 0) set_line_ends(*doc, i == 1);
        }, nullptr);
    } else {  // the language: every one blit knows of, by its name, and plain text first
        std::vector<std::string> ids;
        for (const GrammarSource& g : grammar_list(cfg_.grammars)) ids.push_back(g.name);
        std::sort(ids.begin(), ids.end(), [this](const std::string& a, const std::string& b) {
            return SDL_strcasecmp(language_name(a, cfg_.grammars).c_str(), language_name(b, cfg_.grammars).c_str()) < 0;
        });
        ids.insert(ids.begin(), "-");
        const std::string now = file_language();
        // (Its grammar missing: getting it is the first row.)
        const bool get = grammar_missing() && std::ranges::find(ids, now) != ids.end();
        std::vector<std::string> items;
        size_t at = 0;
        if (get) items.push_back("Install the " + language_name(now, cfg_.grammars) + " grammar");
        for (const std::string& lang : ids) {
            if (lang == (now.empty() ? "-" : now)) at = items.size();
            items.push_back(lang == "-" ? std::string("Plain text") : language_name(lang, cfg_.grammars));
        }
        find_preview_item("Language", std::move(items), at, nullptr, [this, id, ids, get](int i) {
            Doc* doc = find_doc(id);
            if (!doc || i < 0) return;
            if (get && i == 0) return grammar_install();
            set_language(*doc, ids[static_cast<size_t>(i - (get ? 1 : 0))]);
        }, nullptr);
    }
}

// A file's line ends made all one kind ("\r\n", or "\n"), as one step to undo;
// what Enter puts in from then on is that kind.
void App::set_line_ends(Doc& d, bool crlf) {
    if (d.read_only || d.term || d.prof || d.image || d.pdf) return;
    Editor& ed = d.ed;
    const std::string text = ed.buf.text(0, ed.buf.size());
    std::string out;
    out.reserve(text.size() + (crlf ? text.size() / 32 : 0));
    const u64 cursor = ed.cursor();
    u64 moved_to = cursor;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == cursor) moved_to = out.size();
        const char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;  // (the \n after it says what goes)
        if (c == '\n' && crlf) out += '\r';
        out += c;
    }
    if (cursor >= text.size()) moved_to = out.size();
    ed.set_crlf(crlf);
    if (out != text) {
        ed.replace(0, ed.buf.size(), out);
        ed.move_to(std::min<u64>(moved_to, ed.buf.size()), false);
        if (pane_->doc == &d) vim_mouse();  // (Vim takes the cursor where it is)
    }
    pane_->cache_rev = ~0ull;
    dirty_ = true;
}

// A file's language, chosen ("-": plain text): its colours and what the
// scripts are told (FileType) follow; its language server stays the one
// its name says.
void App::set_language(Doc& d, const std::string& lang) {
    d.lang = lang;
    Doc* const front = pane_->doc;
    const double top = front == &d ? pane_->scroll_y : d.scroll_y;
    pane_->doc = &d;  // (its colours start again, wherever it is)
    stop_syntax();
    start_syntax(static_cast<u64>(std::max(0.0, top)) / static_cast<u64>(font_->cell_h()));
    pane_->doc = front;
    if (script_) script_filetype(d.id);
    grammars_pending_ = true;  // (auto_install may want this one's grammar)
    pick_theme();
    dirty_ = true;
}

// What the bar shows, as text (for the tests).
std::string App::status_dump() const {
    if (!status_ || !status_px()) return "no status bar";
    std::string out;
    for (const Seg& s : status_->left) out += (out.empty() || s.gap == 0 ? "" : " ") + s.text;
    out += " |";
    for (const Seg& s : status_->right) out += " " + s.text;
    return out;
}

}  // namespace blit
