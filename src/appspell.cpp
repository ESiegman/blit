// Spelling (spell.hpp), in English: the words of the comments and strings
// on show (as the syntax colours have them), of a Markdown file's text,
// or of all of a plain text file, checked on a thread of their own once
// the text and the view have been still a moment; each word not known
// underlined as a problem is. Option+Enter on one: its suggestions, or it
// added to the dictionary.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "spell.hpp"

namespace blit {

struct SpellWork {
    struct Ask {
        u64 doc = 0, tag = 0;
        std::vector<std::pair<u64, std::string>> words;  // where each starts, and it
    };
    struct Answer {
        u64 doc = 0, tag = 0;
        std::vector<std::pair<u64, u32>> wrong;  // where, and how long
    };
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Ask> asks;
    std::vector<Answer> answers;
    bool quit = false, busy = false;
    std::atomic<int> ready{-1};  // the checker: -1 not known yet, 0 none, 1 there
    std::unordered_map<std::string, bool> known;  // (the worker's: each word's answer)
    std::jthread thread;  // (last: it ends before what it uses goes)

    SpellWork() {
        thread = std::jthread([this] {
            ready = spell_ready() ? 1 : 0;
            wake();
            for (;;) {
                Ask a;
                {
                    std::unique_lock lock(mu);
                    cv.wait(lock, [this] { return quit || !asks.empty(); });
                    if (quit) return;
                    a = std::move(asks.back());  // (the newest: those before it are out of date)
                    asks.clear();
                    busy = true;
                    for (const std::string& w : learned) known.erase(w);  // (added to the dictionary since)
                    learned.clear();
                }
                Answer out{a.doc, a.tag, {}};
                if (ready == 1)
                    for (const auto& [at, w] : a.words) {
                        auto it = known.find(w);
                        if (it == known.end()) it = known.emplace(w, spell_ok(w)).first;
                        if (!it->second) out.wrong.emplace_back(at, static_cast<u32>(w.size()));
                    }
                {
                    std::lock_guard lock(mu);
                    answers.push_back(std::move(out));
                    busy = false;
                }
                wake();
            }
        });
    }
    ~SpellWork() {
        {
            std::lock_guard lock(mu);
            quit = true;
        }
        cv.notify_all();
    }
    static void wake() {
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 24;
        SDL_PushEvent(&ev);
    }
    void ask(Ask a) {
        {
            std::lock_guard lock(mu);
            asks.push_back(std::move(a));
        }
        cv.notify_one();
    }
    void forget(const std::string& w) {  // (added to the dictionary: its answer asked again)
        std::lock_guard lock(mu);
        learned.push_back(w);
    }
    std::vector<std::string> learned;  // (mu)
};

struct SpellUi {
    u64 doc = 0, rev = ~0ull, tag = 0, due_ns = 0;
    u64 key[4] = {};                        // (the view the words were asked for: top, bottom, colours)
    std::vector<std::pair<u64, u32>> wrong;  // in the document as of rev
    std::vector<GridDeco> decos;
};

// Which of a line's bytes are prose to check: in code, comments and
// strings; in Markdown, text that is not code or an address; in a file
// with no colours, all of it.
namespace {

enum Prose : u8 { kProseNone, kProseCode, kProseMarkdown, kProseAll };

bool prose_style(Prose kind, std::string_view name) {
    if (kind == kProseCode) return name.starts_with("comment") || (name.starts_with("string") && !name.starts_with("string.special"));
    return !(name.find("raw") != std::string_view::npos || name.find("url") != std::string_view::npos || name.find("link") != std::string_view::npos ||
             name.starts_with("punctuation") || name.starts_with("label"));
}

}  // namespace

void App::spell_marks(FrameDesc& fd) {
    if (!cfg_.spell || focus_view_ >= fd.view_count || !pane_ || !pane_->doc) return;
    Doc& d = *pane_->doc;
    if (d.read_only || d.term || d.prof || d.md || d.image || d.pdf || d.ed.path.empty()) return;
    if (!spell_work_) spell_work_ = std::make_shared<SpellWork>();
    if (spell_work_->ready == 0) return;
    if (!spell_) spell_ = std::make_shared<SpellUi>();
    SpellUi& u = *spell_;
    GridSpec& g = fd.views[focus_view_].grid;
    const u64 now = now_ns();
    if (u.doc != d.id) {
        u = SpellUi{};
        u.doc = d.id;
    }
    if (u.rev != d.ed.revision()) {  // (changed: what is past the change is not known any more)
        const u64 low = u.rev == ~0ull ? 0 : d.ed.edit_log().changed_since(u.rev, d.ed.revision());
        std::erase_if(u.wrong, [&](const std::pair<u64, u32>& w) { return w.first + w.second >= low; });
        u.rev = d.ed.revision();
        u.due_ns = now + 300'000'000ull;
    }
    const u64 key[4] = {g.top_line, g.bottom(), d.hl_gen, d.ed.revision()};
    if (!std::equal(std::begin(key), std::end(key), std::begin(u.key))) {
        std::copy(std::begin(key), std::end(key), std::begin(u.key));
        u.due_ns = std::max(u.due_ns, now + 100'000'000ull);  // (once the view has been still a moment: not while it scrolls)
    }
    if (u.due_ns && now >= u.due_ns && spell_work_->ready == 1) {
        u.due_ns = 0;
        const std::string lang = file_language();
        const Prose kind = lang == "markdown" ? kProseMarkdown : d.syntax.active() ? kProseCode : (lang.empty() || lang == "text") ? kProseAll : kProseNone;
        if (kind != kProseNone) {
            SpellWork::Ask a;
            a.doc = d.id;
            a.tag = ++u.tag;
            const Highlights& hl = d.syntax.active() ? d.syntax.highlights() : Highlights();
            const std::vector<std::string>& names = d.syntax.style_names();
            const Buffer& b = d.ed.buf;
            for (u64 l = g.top_line; l < g.bottom() && l < b.line_count(); ++l) {
                const u64 s = b.line_start(l), e = d.ed.line_end(l);
                if (e - s > 2000) continue;
                const std::string text = b.text(s, e - s);
                // The runs of prose on the line, each looked through for words.
                size_t m = hl.empty() ? 0 : hl.upper(s);
                u8 st = m && !hl.empty() ? hl.style[m - 1] : 0;
                size_t run = std::string::npos;
                auto words = [&](size_t a0, size_t a1) {
                    for (const auto& [o, n] : spell_words(std::string_view(text).substr(a0, a1 - a0))) a.words.emplace_back(s + a0 + o, text.substr(a0 + o, n));
                };
                bool ticked = false;  // (Markdown: inside `code`, which is not prose whatever its colour)
                for (size_t i = 0; i <= text.size(); ++i) {
                    if (!hl.empty())
                        while (m < hl.size() && hl.at(m) <= s + i) st = hl.style[m++];
                    if (kind == kProseMarkdown && i < text.size() && text[i] == '`') ticked = !ticked;
                    const bool in = i < text.size() && !ticked && text[i] != '`' && (kind == kProseAll || (kind == kProseMarkdown && st == 0) || (st && st < names.size() && prose_style(kind, names[st])));
                    if (in && run == std::string::npos) run = i;
                    if (!in && run != std::string::npos) {
                        words(run, i);
                        run = std::string::npos;
                    }
                }
            }
            spell_work_->ask(std::move(a));
        }
    }
    // What is known to be wrong, underlined.
    if (u.wrong.empty()) return;
    u.decos.assign(g.decos.begin(), g.decos.end());
    const Buffer& b = d.ed.buf;
    for (const auto& [at, n] : u.wrong) {
        if (at + n > b.size()) continue;
        const u64 l = b.line_of(at);
        if (l < g.top_line || l >= g.bottom()) continue;
        const u64 s = b.line_start(l);
        u.decos.push_back({l, d.ed.col_between(s, at), d.ed.col_between(s, at + n), 3});
    }
    g.decos = u.decos;
}

// The worker has answered: the newest answer for the file in front is what is wrong.
void App::spell_take() {
    if (!spell_work_) return;
    std::vector<SpellWork::Answer> got;
    {
        std::lock_guard lock(spell_work_->mu);
        got.swap(spell_work_->answers);
    }
    if (spell_)
        for (SpellWork::Answer& a : got)
            if (a.doc == spell_->doc && a.tag == spell_->tag) spell_->wrong = std::move(a.wrong);
    dirty_ = true;
}

int App::spell_wait_ms() const {
    if (!spell_ || !spell_->due_ns) return -1;
    const u64 now = now_ns();
    return spell_->due_ns <= now ? 0 : static_cast<int>((spell_->due_ns - now) / 1'000'000 + 1);
}

// Option+Enter on a word underlined as wrong: what it might be, or it
// added to the dictionary. False: the cursor is not on one.
bool App::spell_fix() {
    if (!spell_ || spell_->doc != pane_->doc->id || spell_->rev != pane_->doc->ed.revision()) return false;
    Editor& ed = pane_->doc->ed;
    const u64 cur = ed.cursor();
    for (const auto& [at, n] : spell_->wrong) {
        if (cur < at || cur > at + n) continue;
        const std::string word = ed.buf.text(at, n);
        std::vector<std::string> guesses = spell_suggest(word);
        std::vector<std::string> rows = guesses;
        rows.push_back("Add \xE2\x80\x9C" + word + "\xE2\x80\x9D to the dictionary");
        const u64 doc = pane_->doc->id, rev = ed.revision();
        lsp_close(kPopAll);
        find_choose_item("Spelling of \xE2\x80\x9C" + word + "\xE2\x80\x9D", std::move(rows), [this, guesses, word, at, n, doc, rev](int i) {
            if (i < 0 || pane_->doc->id != doc || pane_->doc->ed.revision() != rev) return;
            if (static_cast<size_t>(i) < guesses.size()) {
                if (cfg_.modal) vim_attach();
                pane_->doc->ed.replace(at, at + n, guesses[static_cast<size_t>(i)]);
            } else {
                spell_learn(word);
                if (spell_work_) spell_work_->forget(word);
                std::erase_if(spell_->wrong, [&](const std::pair<u64, u32>& w) { return pane_->doc->ed.buf.text(w.first, w.second) == word; });
            }
            spell_->due_ns = now_ns();
            dirty_ = true;
        });
        return true;
    }
    return false;
}

// For the tests: the words underlined as wrong in the split in front
// (once the checker has looked), or why there are none.
std::string App::spell_dump() {
    if (!cfg_.spell) return "off";
    for (const u64 until = now_ns() + 5'000'000'000ull; now_ns() < until;) {
        frame();
        bool idle = spell_work_ && spell_work_->ready != -1;
        if (idle && spell_work_) {
            std::lock_guard lock(spell_work_->mu);
            idle = spell_work_->asks.empty() && !spell_work_->busy;
        }
        if (idle && spell_ && !spell_->due_ns) break;
        spell_take();
        SDL_Delay(5);
    }
    spell_take();
    if (!spell_work_ || spell_work_->ready != 1) return "no checker";
    std::string out;
    for (const auto& [at, n] : spell_ ? spell_->wrong : std::vector<std::pair<u64, u32>>{}) out += (out.empty() ? "" : " ") + pane_->doc->ed.buf.text(at, n);
    return out.empty() ? "none" : out;
}

}  // namespace blit
