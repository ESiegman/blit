// The status bar: a row across the bottom of the window, under the splits
// and the side panel. On the left the repository (the branch, how far it is
// ahead of and behind the one it follows, the lines of this file changed
// since the last commit, how many files are changed) and the file (its path
// in the repository, unsaved or read only); on the right what a script put
// there (blit.ui.status), the language server's problems, the cursor's line
// and column, the indentation, the line endings, the language and its
// server. A click on the repository opens the Git tab of the side panel, one
// on the problems the Problems tab.
//
// Its cells are made again only when something it shows has changed; a
// frame that changes nothing in it costs a comparison of a few numbers.
#include <algorithm>
#include <utility>
#include <cstdio>
#include <string>
#include <vector>

#include "app.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

enum Action : u8 { kNone, kGitTab, kProblemsTab, kReconnect };

// One part of the bar: its text, how it is drawn, what a click on it does,
// how many blank columns come before it (after the first), and (on the
// right) how soon it goes when there is not room for all: the lowest first.
struct Seg {
    std::string text;
    u32 ink = kInkMuted;
    u32 style = 0;
    Action action = kNone;
    u32 gap = 2;
    int keep = 0;
    bool shrink = false;  // a path's folders: shortened from the start before anything goes
};

// A language's name as people write it ("C++" for "cpp"); one not listed
// with a capital.
std::string language_name(const std::string& id) {
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

std::string count_text(u64 n) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(n));
    return b;
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
    };
    std::vector<Spot> spots;
    // The language of a file, kept for it (it looks at the file's name and first line).
    u64 lang_doc = ~0ull;
    std::string lang_path, lang;
    // The language server's problems, counted once for each set of them.
    u64 diag_gen = ~0ull;
    size_t errors = 0, warnings = 0;
};

int App::status_px() const {
    if (!cfg_.status_bar || !font_) return 0;
    return font_->cell_h() + font_->cell_h() / 4;
}

void App::status_layout(int w, int h, FrameDesc& fd) {
    const int tall = status_px();
    if (!tall || fd.popup_count >= std::size(fd.popups)) return;
    if (!status_) status_ = std::make_shared<StatusUi>();
    StatusUi& u = *status_;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h(), pad_x = cw / 2 + 1;
    const u32 cols = static_cast<u32>(std::max(1, (w - 2 * pad_x) / cw));
    Doc& d = *pane_->doc;
    const Editor& ed = d.ed;

    // Everything it shows, in a few numbers: when they are as they were, so are the cells.
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    mix(d.id);
    mix(ed.revision());
    mix(ed.modified() ? 1 : 0);
    mix(d.read_only ? 1 : 0);
    mix(d.loading ? 1 : 0);
    mix(remote_gen_);
    mix(std::hash<std::string>{}(ed.path));
    mix(pane_->cur_line);
    mix(pane_->cur_col);
    mix(ed.anchor() ^ (ed.cursor() << 1));
    u64 vlo = 0, vhi = 0;
    const bool visual = vim_selection(&vlo, &vhi);
    mix(visual ? vlo * 31 + vhi : 0);
    mix(git_gen_);
    mix(d.hunks_gen);
    mix(lsp_.attached() ? lsp_.diagnostics_generation() * 4 + (lsp_.ready() ? 2 : 1) : 0);
    mix(std::hash<std::string>{}(script_status_));
    mix(d.indent_tabs ? indent_step() + 1000 : indent_step());
    mix(cols);
    mix(font.generation());
    if (key != u.key) {
        u.key = key;
        u.left.clear();
        u.right.clear();
        const GitSummary g = git_summary();  // (git_gen_ above says when this changes)

        // The machine, when the file or the folder is on another one, and how its connection is.
        {
            const std::string* root = panel_root();
            std::string host, there;
            if (split_remote(ed.path, &host, &there) || (root && split_remote(*root, &host, &there))) {
                std::shared_ptr<Remote> r;
                for (const std::shared_ptr<Remote>& c : Remote::all())
                    if (c->host() == host) r = c;
                const Remote::State st = r ? r->state() : Remote::kConnecting;
                const char* icon = font.has(0x21C4) ? "\xE2\x87\x84 " : "";
                Seg s;
                s.action = kNone;
                if (st == Remote::kReady) {
                    s.text = icon + host;
                    s.ink = kInkAccent;
                } else if (st == Remote::kFailed) {
                    s.text = icon + host + ": not connected (connect again)";
                    s.ink = kInkDiagnostic;
                    s.action = kReconnect;
                } else {
                    const std::string doing = r ? r->progress() : std::string();
                    s.text = icon + (doing.empty() ? "connecting to " + host : doing) + "\xE2\x80\xA6";
                    s.ink = kInkMuted;
                }
                u.left.push_back(std::move(s));
            }
        }

        // The repository.
        if (g.repo) {
            const char* branch_icon = font.has(0xE0A0) ? "\xEE\x82\xA0 " : font.has(0x2387) ? "\xE2\x8E\x87 " : "";
            Seg s;
            s.text = std::string(branch_icon) + (g.branch.empty() ? "(" + g.head + ")" : g.branch);
            s.ink = kInkAccent;
            s.action = kGitTab;
            if (g.ahead > 0) s.text += std::string(font.has(0x2191) ? " \xE2\x86\x91" : " +") + count_text(static_cast<u64>(g.ahead));
            if (g.behind > 0) s.text += std::string(font.has(0x2193) ? " \xE2\x86\x93" : " -") + count_text(static_cast<u64>(g.behind));
            if (g.merging) s.text += " merging";
            u.left.push_back(std::move(s));
            // This file's lines that differ from the last commit.
            u64 added = 0, changed = 0, removed = 0;
            for (const GitHunk& k : d.hunks) {
                if (k.kind == GitHunk::kAdded) added += k.count;
                else if (k.kind == GitHunk::kModified) changed += k.count;
                else removed += k.removed;
            }
            u32 gap = 2;  // (one between the counts)
            if (added) u.left.push_back({"+" + count_text(added), kInkAdded, 0, kGitTab, std::exchange(gap, 1u)});
            if (changed) u.left.push_back({"~" + count_text(changed), kInkChanged, 0, kGitTab, std::exchange(gap, 1u)});
            if (removed) u.left.push_back({(font.has(0x2212) ? "\xE2\x88\x92" : "-") + count_text(removed), kInkDiagnostic, 0, kGitTab, std::exchange(gap, 1u)});
            if (g.changed) u.left.push_back({count_text(g.changed) + (g.changed == 1 ? " file changed" : " files changed"), kInkMuted, 0, kGitTab});
        }
        // The file: its path in the repository (or the working directory).
        {
            std::string path = ed.path.empty() ? (d.title.empty() ? std::string("untitled") : d.title) : full_path(ed.path);
            const std::string* root = panel_root();
            const std::string base = g.repo ? g.root : root ? *root : std::string();
            std::string host, there;
            if (!ed.path.empty() && !base.empty() && path.size() > base.size() + 1 && path.starts_with(base) && path[base.size()] == '/') {
                path = path.substr(base.size() + 1);
            } else if (split_remote(path, &host, &there)) {  // (on another machine, whose name is shown already: ~ for the home folder there)
                path = there;
                for (const std::shared_ptr<Remote>& c : Remote::all())
                    if (const std::string home_dir = c->host() == host ? c->home() : std::string();
                        !home_dir.empty() && path.size() > home_dir.size() + 1 && path.starts_with(home_dir) && path[home_dir.size()] == '/')
                        path = "~" + path.substr(home_dir.size());
            } else if (const char* home = SDL_getenv("HOME"); !ed.path.empty() && home && *home) {  // (elsewhere: ~ for the home folder)
                const std::string home_dir(home);
                if (path.size() > home_dir.size() + 1 && path.starts_with(home_dir) && path[home_dir.size()] == '/') path = "~" + path.substr(home_dir.size());
            }
            const size_t slash = path.rfind('/');
            const u32 gap = g.repo ? 3 : 2;  // (a little more between the repository and the file)
            if (slash != std::string::npos) u.left.push_back({path.substr(0, slash + 1), kInkMuted, 0, kNone, gap, 0, true});
            u.left.push_back({slash == std::string::npos ? path : path.substr(slash + 1), 0, Font::kBold, kNone, slash == std::string::npos ? gap : 0});
            if (d.loading) u.left.push_back({"loading\xE2\x80\xA6", kInkMuted, 0, kNone, 2});
            else if (d.read_only && !d.term && !d.prof && !d.image) u.left.push_back({"read only", kInkMuted, 0, kNone, 2});
            else if (ed.modified()) u.left.push_back({font.has(0x25CF) ? "\xE2\x97\x8F" : "*", kInkAccent, 0, kNone, 1});
        }

        // On the right, what is least needed first to go.
        if (!script_status_.empty()) u.right.push_back({script_status_, 0, 0, kNone, 2, 1});
        if (lsp_.attached()) {
            if (u.diag_gen != lsp_.diagnostics_generation()) {
                u.diag_gen = lsp_.diagnostics_generation();
                u.errors = u.warnings = 0;
                for (const Diagnostic& x : lsp_.diagnostics()) {
                    if (x.severity == 1) ++u.errors;
                    else if (x.severity == 2) ++u.warnings;
                }
            }
            const char* err = font.has(0x2715) ? "\xE2\x9C\x95 " : "\xC3\x97 ";                                       // ✕, or ×
            const char* warn = font.has(0x26A0) ? "\xE2\x9A\xA0 " : font.has(0x25B2) ? "\xE2\x96\xB2 " : "! ";  // ⚠, or ▲
            u.right.push_back({err + count_text(u.errors), u.errors ? kInkDiagnostic : kInkMuted, 0, kProblemsTab, 2, 4});
            u.right.push_back({warn + count_text(u.warnings), u.warnings ? kInkDiagnostic + 1 : kInkMuted, 0, kProblemsTab, 1, 4});
        }
        const bool text = !d.term && !d.prof && !d.image;  // (a terminal's tab, a profile's and a picture's have no lines and columns of their own)
        if (text) {
            std::string at = "Ln " + count_text(pane_->cur_line + 1) + ", Col " + count_text(static_cast<u64>(pane_->cur_col) + 1);
            u64 lo = visual ? vlo : std::min(ed.anchor(), ed.cursor()), hi = visual ? vhi : std::max(ed.anchor(), ed.cursor());
            if (hi > lo) {
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
            u.right.push_back({std::move(at), kInkMuted, 0, kNone, 2, 5});
        }
        const u32 step = text ? indent_step() : 0;
        if (text) u.right.push_back({d.indent_tabs ? std::string("Tabs") : "Spaces: " + count_text(step), kInkMuted, 0, kNone, 2, 2});
        if (text) u.right.push_back({ed.crlf() ? "CRLF" : "LF", kInkMuted, 0, kNone, 2, 2});
        if (u.lang_doc != d.id || u.lang_path != ed.path) {
            u.lang_doc = d.id;
            u.lang_path = ed.path;
            u.lang = file_language();
        }
        if (text) {
            std::string lang = u.lang.empty() ? std::string("Plain text") : language_name(u.lang);
            if (lsp_.attached() && !lsp_.server_name().empty()) lang += " \xC2\xB7 " + lsp_.server_name() + (lsp_.ready() ? "" : "\xE2\x80\xA6");
            u.right.push_back({std::move(lang), kInkMuted, 0, kNone, 2, 3});
        }

        // Laid out: the left from the start, the right against the end. What
        // does not fit goes: the right's least needed parts first (the
        // left is then cut off at the end of the bar).
        auto width = [](const std::vector<Seg>& v) {
            u32 n = 0;
            for (size_t i = 0; i < v.size(); ++i) n += width_of(v[i].text) + (i ? v[i].gap : 0);
            return n;
        };
        const u32 room = cols > 2 ? cols - 2 : cols;  // (a blank column at either end)
        auto over = [&] { return static_cast<i64>(width(u.left)) + static_cast<i64>(width(u.right)) + 3 - static_cast<i64>(room); };
        // First the path's folders, from their start ("…/src/"), and if
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
        u32 c = 1;
        for (size_t i = 0; i < u.left.size(); ++i) {
            const Seg& s = u.left[i];
            if (i > 0) c += s.gap;
            const u32 c0 = c;
            c = u.box.put(font, 0, c, s.text, s.ink, s.style);
            if (s.action != kNone) u.spots.push_back({c0, c, s.action});
        }
        const u32 right_w = width(u.right);
        c = cols > right_w + 1 ? cols - 1 - right_w : 0;
        for (size_t i = 0; i < u.right.size(); ++i) {
            const Seg& s = u.right[i];
            if (i > 0) c += s.gap;
            const u32 c0 = c;
            c = u.box.put(font, 0, c, s.text, s.ink, s.style);
            if (s.action != kNone) u.spots.push_back({c0, c, s.action});
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
}

bool App::status_mouse_down(const SDL_MouseButtonEvent& b) {
    const int tall = status_px();
    if (!tall || !status_) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (py < px_h_ - tall) return false;
    if (b.button == SDL_BUTTON_LEFT) {
        const int cw = font_->cell_w(), pad_x = cw / 2 + 1;
        const u32 col = px > pad_x ? static_cast<u32>((px - pad_x) / cw) : 0;
        for (const StatusUi::Spot& s : status_->spots)
            if (col >= s.c0 && col < s.c1) {
                if (s.action == kGitTab) panel_show_git();
                else if (s.action == kProblemsTab) panel_show_problems();
                else if (s.action == kReconnect) remote_reconnect();
                break;
            }
    }
    dirty_ = true;
    return true;  // (the bar has the click, whatever it was on)
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
