// Headless benchmarks for the text core: no window, no GPU.
//   blit_bench gen DIR          write the test corpus
//   blit_bench run FILE [name]  run the benchmarks on FILE, one JSON line each
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "editor.hpp"
#include "finder.hpp"
#include "font.hpp"
#include "git.hpp"
#include "grid.hpp"
#include "json.hpp"
#include "lsp.hpp"
#include "scan.hpp"
#include "syntax.hpp"

using namespace blit;

namespace {

u64 rng_state = 0x243F6A8885A308D3ull;
u64 rnd() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

double rss_mb() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

volatile u64 sink;

// Runs `op` in batches until enough time has passed; reports ns per op.
template <class Op>
double per_op(u64 min_ops, Op&& op) {
    u64 done = 0;
    const u64 t0 = now_ns();
    u64 t1 = t0;
    while (done < min_ops || t1 - t0 < 200'000'000) {
        for (int i = 0; i < 256; ++i) op();
        done += 256;
        t1 = now_ns();
    }
    return static_cast<double>(t1 - t0) / static_cast<double>(done);
}

void emit(const char* file, const char* name, const char* unit, double value) {
    const char* base = std::strrchr(file, '/');
    std::printf("{\"file\":\"%s\",\"bench\":\"%s\",\"unit\":\"%s\",\"value\":%.3f}\n", base ? base + 1 : file, name, unit,
                value);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- corpus

void write_file(const std::string& path, const std::string& unit, size_t bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::perror(path.c_str());
        std::exit(1);
    }
    size_t done = 0;
    while (done < bytes) {
        size_t n = std::min(unit.size(), bytes - done);
        std::fwrite(unit.data(), 1, n, f);
        done += n;
    }
    std::fclose(f);
    std::printf("wrote %s (%zu MB)\n", path.c_str(), bytes >> 20);
}

int gen(const char* dir) {
    std::string d = dir;
    // Source-code-like text: short indented lines.
    std::string code;
    static const char* const lines[] = {
        "static int handle_request(struct conn *c, const struct request *req)\n",
        "{\n",
        "    struct response rsp = {0};\n",
        "    if (!req || req->len == 0) {\n",
        "        log_warn(\"empty request from %s\", c->peer);\n",
        "        return -EINVAL;\n",
        "    }\n",
        "\n",
        "    for (size_t i = 0; i < req->nheaders; i++) {\n",
        "        const struct header *h = &req->headers[i];\n",
        "        if (strcasecmp(h->name, \"content-length\") == 0)\n",
        "            rsp.body_len = strtoull(h->value, NULL, 10);\n",
        "    }\n",
        "\t/* tabs appear too */\n",
        "    return send_response(c, &rsp);\n",
        "}\n",
        "\n",
    };
    while (code.size() < (1u << 20))
        for (const char* l : lines) code += l;
    write_file(d + "/code-1m.txt", code, 1u << 20);
    write_file(d + "/code-100m.txt", code, 100u << 20);

    // Log-like text: ~120 column lines.
    std::string log;
    for (int i = 0; log.size() < (4u << 20); ++i) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "2026-11-14T10:%02d:%02d.%03dZ INFO  [worker-%02d] request id=%08x path=/api/v2/items/%d "
                      "status=200 bytes=%d dur=%dms\n",
                      i / 60 % 60, i % 60, i * 7 % 1000, i % 32, i * 2654435761u, i % 9973, 512 + i % 4096, i % 250);
        log += buf;
    }
    write_file(d + "/log-1g.txt", log, 1u << 30);

    // One enormous line.
    std::string json = "{\"id\":123456,\"name\":\"item\",\"tags\":[\"a\",\"b\",\"c\"],\"value\":3.14159},";
    write_file(d + "/longline-64m.txt", json, 64u << 20);

    // Mixed scripts.
    std::string utf;
    static const char* const ulines[] = {
        "English text mixed with café, naïve, and Ünïcödé.\n",
        "日本語のテキスト：これはテストです。漢字とかなが混ざっています。\n",
        "Русский текст: быстрая коричневая лиса прыгает через ленивую собаку.\n",
        "中文文本：敏捷的棕色狐狸跳过了懒狗。\n",
        "Ελληνικά: γρήγορη καφέ αλεπού. 한국어: 빠른 갈색 여우.\n",
        "math: ∀x ∈ ℝ, x² ≥ 0 ⇒ √(x²) = |x|; arrows → ← ↑ ↓\n",
    };
    while (utf.size() < (1u << 20))
        for (const char* l : ulines) utf += l;
    write_file(d + "/utf8-16m.txt", utf, 16u << 20);
    return 0;
}

// ---------------------------------------------------------------- benchmarks

int run(const char* file, const char* only) {
    auto want = [&](const char* name) { return !only || !std::strcmp(only, name); };

    // Load: best of several, each from scratch (page cache warm).
    double best_ms = 1e30;
    u64 size = 0, lines = 0;
    for (int i = 0; i < 7; ++i) {
        Editor e;
        std::string err;
        const u64 t0 = now_ns();
        if (!e.open(file, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        best_ms = std::min(best_ms, static_cast<double>(now_ns() - t0) * 1e-6);
        size = e.buf.size();
        lines = e.buf.line_count();
    }
    if (want("load")) {
        emit(file, "load_ms", "ms", best_ms);
        emit(file, "load_gb_per_s", "GB/s", static_cast<double>(size) / (1u << 30) / (best_ms * 1e-3));
    }

    Editor ed;
    std::string err;
    ed.open(file, &err);
    Buffer& b = ed.buf;
    if (want("load")) {
        emit(file, "rss_after_load_mb", "MB", rss_mb());
        emit(file, "index_mb", "MB", static_cast<double>(b.index_bytes()) / (1u << 20));
    }

    if (want("line_start"))
        emit(file, "line_start_random_ns", "ns", per_op(200000, [&] { sink = b.line_start(rnd() % lines); }));
    if (want("line_of"))
        emit(file, "line_of_random_ns", "ns", per_op(200000, [&] { sink = b.line_of(rnd() % (size + 1)); }));

    if (want("read")) {
        // Full pass over the text through the reader, touching every byte.
        const u64 t0 = now_ns();
        u64 total = 0, sum = 0;
        for (int rep = 0; rep < 3; ++rep) {
            Buffer::Reader r = b.reader(0);
            for (std::string_view c = r.chunk(); !c.empty(); c = r.chunk()) {
                sum += count_nl(c.data(), c.size());
                total += c.size();
                r.advance(static_cast<u32>(c.size()));
            }
        }
        sink = sum;
        emit(file, "scan_all_gb_per_s", "GB/s",
             static_cast<double>(total) / (1u << 30) / (static_cast<double>(now_ns() - t0) * 1e-9));
    }

    // Grid fill: what one frame costs on the CPU, for a 4K-ish window.
    if (want("fill")) {
        // BLIT_BENCH_FONT picks the font; BLIT_BENCH_LIGATURES=1 turns the
        // ligature pass on, to measure what it costs.
        FontSpec spec;
        if (const char* f = std::getenv("BLIT_BENCH_FONT")) spec.family = f;
        spec.ligatures = std::getenv("BLIT_BENCH_LIGATURES") != nullptr;
        Font font;
        if (!font.init(spec, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (spec.ligatures && !font.ligatures()) std::fprintf(stderr, "note: this font has no ligatures\n");
        const std::string tag = spec.ligatures ? "_ligatures" : "";
        auto emit_fill = [&](const char* name, double us) { emit(file, (std::string(name) + tag).c_str(), "us", us); };
        GridSpec g;
        g.cols = 290;
        g.rows = 82;
        std::vector<u32> cells(static_cast<size_t>(g.cols) * g.rows);
        emit_fill("fill_frame_sequential_us", 1e-3 * per_op(2000, [&] {
                 g.top_line = (g.top_line + 3) % lines;
                 fill_grid(b, font, g, cells.data());
             }));
        emit_fill("fill_frame_random_us", 1e-3 * per_op(2000, [&] {
                 g.top_line = rnd() % lines;
                 fill_grid(b, font, g, cells.data());
             }));
        g.top_line = lines / 2;
        g.has_sel = true;
        g.sel_line0 = 0;
        g.sel_line1 = lines;
        emit_fill("fill_frame_selected_us", 1e-3 * per_op(2000, [&] {
                 g.top_line = (g.top_line + 3) % lines;
                 fill_grid(b, font, g, cells.data());
             }));
        g.has_sel = false;
        g.top_line = 0;
        emit_fill("fill_frame_hscroll_us", 1e-3 * per_op(2000, [&] {
                 g.first_col = (g.first_col + 7) % 4000;
                 fill_grid(b, font, g, cells.data());
             }));
        // With indent lines: every frame at a new place (each row's
        // indentation is read off the cells afresh), and the same rows
        // again (it is remembered).
        g.first_col = 0;
        g.guide_step = 4;
        g.guide_inks = 6;
        GuideCache guides;
        emit_fill("fill_frame_guides_us", 1e-3 * per_op(2000, [&] {
                 g.top_line = (g.top_line + 3) % lines;
                 fill_grid(b, font, g, cells.data());
                 guide_grid(b, g, cells.data(), guides);
             }));
        emit_fill("fill_frame_guides_same_rows_us", 1e-3 * per_op(2000, [&] {
                 fill_grid(b, font, g, cells.data());
                 guide_grid(b, g, cells.data(), guides);
             }));
        // Scrolled a column sideways, the cells no longer show where each
        // line starts, and its indentation is read from the text instead.
        g.first_col = 1;
        emit_fill("fill_frame_guides_from_text_us", 1e-3 * per_op(2000, [&] {
                 g.top_line = (g.top_line + 3) % lines;
                 fill_grid(b, font, g, cells.data());
                 guide_grid(b, g, cells.data(), guides);
             }));
        g.guide_step = 0;
        emit_fill("fill_frame_one_column_in_us", 1e-3 * per_op(2000, [&] {
                 g.top_line = (g.top_line + 3) % lines;
                 fill_grid(b, font, g, cells.data());
             }));
        sink = cells[0];
    }

    // Git's part that is blit's own work: a number per line of the text, and
    // the comparison of two such lists (the text against its committed
    // version) that gives the marks beside changed lines. Both run on git's
    // thread, not the editor's.
    if (want("git")) {
        std::vector<Piece> pieces;
        b.snapshot(&pieces);
        const u64 t0 = now_ns();
        const std::vector<u64> base = line_hashes(std::span<const Piece>(pieces));
        const double hash_ms = static_cast<double>(now_ns() - t0) * 1e-6;
        emit(file, "git_line_hashes_ms", "ms", hash_ms);
        emit(file, "git_line_hashes_gb_per_s", "GB/s", static_cast<double>(size) / (1u << 30) / (hash_ms * 1e-3));
        // Twenty changes spread through the file: lines altered, a few taken out, a few put in.
        std::vector<u64> now = base;
        const size_t stride = std::max<size_t>(1, now.size() / 21);
        for (size_t k = 1; k <= 20 && k * stride < now.size(); ++k) {
            const auto at = now.begin() + static_cast<std::ptrdiff_t>(k * stride);
            if (k % 3 == 0) now.erase(at);
            else if (k % 3 == 1) now.insert(at, 3, 0x1234567 + k);
            else *at ^= 1;
        }
        size_t hunks = 0;
        emit(file, "git_diff_20_changes_us", "us", 1e-3 * per_op(base.size() > 1000000 ? 3 : 50, [&] { hunks = diff_lines(base, now).size(); }));
        emit(file, "git_diff_hunks", "count", static_cast<double>(hunks));
        emit(file, "git_diff_unchanged_us", "us", 1e-3 * per_op(base.size() > 1000000 ? 3 : 50, [&] { sink = diff_lines(base, base).size(); }));
    }

    // What the view asks of a whole text: its widest line, which limits
    // sideways scrolling (measured when the view is first scrolled sideways,
    // on a thread of its own for a big file), and its indentation step.
    if (want("view")) {
        std::vector<Piece> pieces;
        b.snapshot(&pieces);
        u64 cols = 0;
        const u64 t0 = now_ns();
        cols = Editor::widest_columns(pieces, 4);
        const double ms = static_cast<double>(now_ns() - t0) * 1e-6;
        emit(file, "widest_line_ms", "ms", ms);
        emit(file, "widest_line_gb_per_s", "GB/s", static_cast<double>(size) / (1u << 30) / (ms * 1e-3));
        emit(file, "widest_line_columns", "count", static_cast<double>(cols));
        emit(file, "guess_indent_us", "us", 1e-3 * per_op(200, [&] { sink = ed.guess_indent(); }));
    }

    // Finding files and text (finder.cpp). All of it runs on the finder's
    // own threads; these are the pieces, one thread each, so the numbers say
    // what a search costs, not how many cores there are.
    if (want("find")) {
        // Names: 200,000 paths of the usual shape, ranked against what might be typed.
        FileIndex ix;
        static const char* const dirs[] = {"src", "lib", "include", "tests", "tools", "docs", "third_party", "cmd"};
        static const char* const parts[] = {"widget", "cache", "render", "buffer", "parser", "index", "socket", "layout", "shader", "editor", "config", "stream"};
        static const char* const ends[] = {".cpp", ".hpp", ".c", ".h", ".py", ".rs", ".md", ".txt"};
        for (u32 i = 0; i < 200000; ++i) {
            ix.offs.push_back(static_cast<u32>(ix.names.size()));
            ix.names += std::string(dirs[i % 8]) + "/module" + std::to_string(i % 97) + "/" + parts[(i / 8) % 12] + "_" + parts[(i / 96) % 12] + "_" + std::to_string(i % 1000) +
                        ends[(i / 7) % 8];
        }
        ix.offs.push_back(static_cast<u32>(ix.names.size()));
        emit(file, "find_names_paths", "count", static_cast<double>(ix.size()));
        emit(file, "find_names_bytes", "bytes", static_cast<double>(ix.names.size()));
        struct Query {
            const char* typed;
            const char* name;
        };
        for (const Query q : {Query{"wc", "find_names_2_letters_ms"}, Query{"widcach", "find_names_7_letters_ms"}, Query{"src render 42 cpp", "find_names_4_words_ms"},
                              Query{"zqzqzq", "find_names_no_match_ms"}}) {
            const std::vector<FuzzyTerm> terms = fuzzy_terms(q.typed);
            FindResults r;
            emit(file, q.name, "ms", 1e-6 * per_op(5, [&] { Finder::rank_names(ix, terms, &r); }));
            emit(file, (std::string(q.name) + "_matched").c_str(), "count", static_cast<double>(r.matched));
        }
        // Text: this file's own text (the first 256 MB of it), searched for
        // words that are not there (how fast it is passed over: one with a
        // rare letter in it, one of common letters, one with a capital), and
        // for one that is on many lines (what a line found costs).
        const u64 take = std::min<u64>(size, 256u << 20);
        const std::string text = b.text(0, take);
        std::vector<FindHit> hits;
        auto pass = [&](const char* typed, const char* name) {
            const std::vector<FuzzyTerm> terms = fuzzy_terms(typed);
            hits.clear();
            const u64 t0 = now_ns();
            Finder::grep(text, terms, 0, 1u << 30, &hits);
            const double ms = static_cast<double>(now_ns() - t0) * 1e-6;
            emit(file, (std::string(name) + "_gb_per_s").c_str(), "GB/s", static_cast<double>(take) / (1u << 30) / (ms * 1e-3));
            return ms;
        };
        pass("qzjxvkqzjx", "find_text_absent_rare_letters");
        pass("tensor", "find_text_absent_common_letters");
        pass("Tensor", "find_text_absent_exact_case");
        pass("tensor shape", "find_text_absent_two_words");
        const double common_ms = pass("return", "find_text_common_word");
        emit(file, "find_text_common_word_lines", "count", static_cast<double>(hits.size()));
        emit(file, "find_text_common_word_ns_per_line", "ns", hits.empty() ? 0.0 : common_ms * 1e6 / static_cast<double>(hits.size()));
    }

    // Syntax highlighting: what the worker thread spends, how long colours
    // take to follow an edit, and what highlighting adds on the main thread.
    if (want("syntax")) {
        const std::string lang = Syntax::language_for(file, "");
        SyntaxOptions so;
        if (const char* m = std::getenv("BLIT_BENCH_MARGIN")) so.margin_lines = static_cast<u32>(std::atoi(m));
        Editor e;
        e.open(file, &err);
        Syntax syn;
        const u64 mid = e.buf.line_count() / 2;
        const u64 t0 = now_ns();
        if (syn.attach(e.buf, lang, so)) {
            e.listen(&syn);
            syn.want(e.buf, mid, mid + 80);
            syn.wait_idle();
            syn.poll();
            const double first_ms = static_cast<double>(now_ns() - t0) * 1e-6;
            SyntaxStats st = syn.stats();
            emit(file, "syntax_setup_ms", "ms", st.setup_ms);
            emit(file, "syntax_first_parse_ms", "ms", st.parse_ms);
            emit(file, "syntax_first_parse_mb_per_s", "MB/s", static_cast<double>(size) / (1u << 20) / (st.parse_ms * 1e-3));
            emit(file, "syntax_window_query_ms", "ms", st.query_ms);
            emit(file, "syntax_window_marks", "count", static_cast<double>(st.marks));
            emit(file, "syntax_open_to_colours_ms", "ms", first_ms);

            // Typing in the middle of the file, one character at a time.
            static const char text[] = "int value = compute(x, y) + 1; // note\n";
            e.move_to(e.buf.line_start(mid + 40), false);
            double main_ns = 0, parse_ms = 0, query_ms = 0, worst_ms = 0, turn_ms = 0, vis_ms = 0;
            const int edits = 200;
            for (int i = 0; i < edits; ++i) {
                const u64 a = now_ns();
                e.insert(std::string_view(&text[static_cast<size_t>(i) % (sizeof text - 1)], 1));
                syn.want(e.buf, mid, mid + 80);
                const u64 sent = now_ns();
                syn.wait_idle();
                const u64 c = now_ns();
                syn.poll();
                st = syn.stats();
                main_ns += static_cast<double>(sent - a);
                parse_ms += st.parse_ms;
                query_ms += st.query_ms;
                vis_ms += st.parse_ms + st.visible_ms;
                const double turn = static_cast<double>(c - a) * 1e-6;
                turn_ms += turn;
                worst_ms = std::max(worst_ms, turn);
            }
            emit(file, "syntax_key_main_thread_us", "us", main_ns * 1e-3 / edits);
            emit(file, "syntax_key_reparse_ms", "ms", parse_ms / edits);
            emit(file, "syntax_key_requery_ms", "ms", query_ms / edits);
            emit(file, "syntax_key_to_visible_colours_ms", "ms", vis_ms / edits);
            emit(file, "syntax_key_to_all_colours_ms", "ms", turn_ms / edits);
            emit(file, "syntax_key_to_all_colours_worst_ms", "ms", worst_ms);

            // The main thread's side while the worker is busy: an edit plus
            // the hand-off, without waiting for the result.
            emit(file, "syntax_key_main_thread_burst_us", "us", 1e-3 * per_op(2000, [&] {
                     e.insert("x");
                     syn.want(e.buf, mid, mid + 80);
                     syn.poll();
                 }));
            syn.wait_idle();
            syn.poll();

            // Filling the grid with colours, against the same rows without.
            // BLIT_BENCH_FONT and BLIT_BENCH_LIGATURES work as for "fill".
            Font font;
            FontSpec spec;
            if (const char* f = std::getenv("BLIT_BENCH_FONT")) spec.family = f;
            spec.ligatures = std::getenv("BLIT_BENCH_LIGATURES") != nullptr;
            if (!font.init(spec, &err)) return 1;
            u32 lut[256];
            for (u32 i = 0; i < 256; ++i) lut[i] = (i % 31 + 1) << kCellInkShift;
            GridSpec g;
            g.cols = 290;
            g.rows = 82;
            std::vector<u32> cells(static_cast<size_t>(g.cols) * g.rows);
            const u64 span = so.margin_lines > 100 ? so.margin_lines - 100 : 1;
            g.top_line = mid;
            emit(file, "fill_frame_plain_same_rows_us", "us", 1e-3 * per_op(2000, [&] {
                     g.top_line = mid + (g.top_line - mid + 3) % span;
                     fill_grid(e.buf, font, g, cells.data());
                 }));
            g.highlights = &syn.highlights();
            g.style_bits = lut;
            g.top_line = mid;
            emit(file, "fill_frame_highlighted_us", "us", 1e-3 * per_op(2000, [&] {
                     g.top_line = mid + (g.top_line - mid + 3) % span;
                     fill_grid(e.buf, font, g, cells.data());
                 }));
            // The same with italic and bold in play: every style id also
            // picks one of the four font styles.
            bool styled = true;
            for (u32 fs = 1; fs < Font::kStyles; ++fs) styled = font.ensure_style(fs) && styled;
            if (styled) {
                for (u32 i = 0; i < 256; ++i) lut[i] |= (i % Font::kStyles) << Font::kStyleShift;
                g.top_line = mid;
                emit(file, "fill_frame_styled_us", "us", 1e-3 * per_op(2000, [&] {
                         g.top_line = mid + (g.top_line - mid + 3) % span;
                         fill_grid(e.buf, font, g, cells.data());
                     }));
            }
            sink = cells[0];
            e.unlisten(&syn);
            syn.detach();
        } else if (only) {
            std::fprintf(stderr, "no grammar for this file\n");
        }
    }

    // Edits. Each starts from a fresh load so results do not depend on order.
    if (want("type")) {
        Editor e;
        e.open(file, &err);
        e.move_to(e.buf.line_start(e.buf.line_count() / 2), false);
        u64 i = 0;
        static const char text[] = "the quick brown fox jumps over the lazy dog\n";
        emit(file, "type_char_ns", "ns",
             per_op(500000, [&] { e.insert(std::string_view(&text[i++ % (sizeof text - 1)], 1)); }));
    }
    if (want("type_raw")) {
        Buffer raw;
        raw.load_file(file, &err);
        u64 off = raw.line_start(raw.line_count() / 2), i = 0;
        static const char text[] = "the quick brown fox jumps over the lazy dog\n";
        emit(file, "buffer_insert_at_cursor_ns", "ns",
             per_op(500000, [&] { raw.insert(off++, std::string_view(&text[i++ % (sizeof text - 1)], 1)); }));
    }
    if (want("random_insert")) {
        Buffer raw;
        raw.load_file(file, &err);
        emit(file, "buffer_insert_random_ns", "ns",
             per_op(200000, [&] { raw.insert(rnd() % (raw.size() + 1), "x"); }));
        emit(file, "pieces_after_random_insert", "count", static_cast<double>(raw.piece_count()));
    }
    if (want("random_erase")) {
        Buffer raw;
        raw.load_file(file, &err);
        emit(file, "buffer_erase_random_ns", "ns",
             per_op(200000, [&] { raw.erase(rnd() % raw.size(), 1 + rnd() % 8); }));
    }
    if (want("backspace")) {
        Editor e;
        e.open(file, &err);
        e.move_to(e.buf.size(), false);
        emit(file, "backspace_ns", "ns", per_op(100000, [&] { e.backspace(); }));
    }
    if (want("undo")) {
        Editor e;
        e.open(file, &err);
        e.move_to(e.buf.line_start(e.buf.line_count() / 3), false);
        for (int k = 0; k < 2000; ++k) {
            e.insert("word ");
            if (k % 7 == 0) e.newline();
            if (k % 50 == 0) e.move_to(e.buf.line_start(rnd() % e.buf.line_count()), false);
        }
        const u64 t0 = now_ns();
        u64 n = 0;
        while (e.undo()) ++n;
        while (e.redo()) ++n;
        emit(file, "undo_redo_step_ns", "ns", static_cast<double>(now_ns() - t0) / static_cast<double>(n ? n : 1));
    }
    if (want("select_all_delete")) {
        Editor e;
        e.open(file, &err);
        const u64 t0 = now_ns();
        e.select_all();
        e.backspace();
        const u64 t1 = now_ns();
        e.undo();
        const u64 t2 = now_ns();
        emit(file, "select_all_delete_ms", "ms", static_cast<double>(t1 - t0) * 1e-6);
        emit(file, "undo_delete_all_ms", "ms", static_cast<double>(t2 - t1) * 1e-6);
        if (e.buf.size() != size) {
            std::fprintf(stderr, "undo did not restore the text\n");
            return 1;
        }
    }
    if (want("paste")) {
        Editor e;
        e.open(file, &err);
        std::string clip = e.buf.text(0, std::min<u64>(size, 8u << 20));
        e.move_to(e.buf.size() / 2, false);
        const u64 t0 = now_ns();
        e.insert(clip);
        emit(file, "paste_mb_per_ms", "MB/ms",
             static_cast<double>(clip.size()) / (1u << 20) / (static_cast<double>(now_ns() - t0) * 1e-6));
    }
    if (want("save")) {
        std::string out = std::string(file) + ".bench-out";
        const u64 t0 = now_ns();
        if (!b.save_file(out.c_str(), &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        emit(file, "save_ms", "ms", static_cast<double>(now_ns() - t0) * 1e-6);
        std::remove(out.c_str());
    }
#if defined(BLIT_FAKE_LSP)
    // The language server client, against a server that answers at once and
    // predictably (tests/fake_lsp.cpp). Asked for by name only: it starts a
    // process. What matters is the main thread: what an edit costs with a
    // server listening, and what taking in the server's answers costs.
    if (only && !std::strcmp(only, "lsp")) {
        auto wait_for = [](Lsp& l, u32 kinds, int ms = 10000) {
            const u64 until = now_ns() + static_cast<u64>(ms) * 1'000'000;
            u32 got = 0;
            for (;;) {
                got |= l.poll();
                if ((got & kinds) || now_ns() >= until) return got;
                l.wait(5);
            }
        };
        auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v.empty() ? 0.0 : v[v.size() / 2];
        };
        for (const bool utf16 : {false, true}) {
            Editor e;
            e.open(file, &err);
            LspServer fake;
            fake.name = "fake";
            fake.command = {BLIT_FAKE_LSP, "--items", "5000", "--quiet"};
            if (!utf16) fake.command.push_back("--utf8");
            fake.languages = {"cpp"};
            Lsp lsp;
            const u64 t0 = now_ns();
            if (!lsp.attach(e.buf, file, "cpp", {fake})) {
                std::fprintf(stderr, "cannot start the test language server\n");
                return 1;
            }
            const u64 t_attach = now_ns();
            e.listen(&lsp);
            wait_for(lsp, Lsp::kState);
            const u64 t_ready = now_ns();
            wait_for(lsp, Lsp::kDiagnostics);
            const u64 t_diag = now_ns();
            const std::string tag = utf16 ? "_utf16" : "";
            if (!utf16) {
                emit(file, "lsp_attach_main_thread_us", "us", static_cast<double>(t_attach - t0) * 1e-3);
                emit(file, "lsp_start_to_ready_ms", "ms", static_cast<double>(t_ready - t0) * 1e-6);
                emit(file, "lsp_ready_to_diagnostics_ms", "ms", static_cast<double>(t_diag - t_ready) * 1e-6);
                emit(file, "lsp_diagnostics", "count", static_cast<double>(lsp.diagnostics().size()));
            }

            // Typing with the server listening: the edit itself, the change
            // message, and shifting the diagnostics. A fixed number of
            // keystrokes, at a rate no person reaches; the server works
            // through them afterwards.
            e.move_to(e.buf.line_start(e.buf.line_count() / 2), false);
            static const char text[] = "the quick brown fox jumps over the lazy dog\n";
            constexpr u64 kKeys = 20000;
            const u64 k0 = now_ns();
            for (u64 n = 0; n < kKeys; ++n) e.insert(std::string_view(&text[n % (sizeof text - 1)], 1));
            emit(file, ("lsp_keystroke_main_thread_ns" + tag).c_str(), "ns", static_cast<double>(now_ns() - k0) / kKeys);
            // An answer to a question asked now comes after the server has
            // dealt with every keystroke.
            lsp.hover(0);
            wait_for(lsp, Lsp::kHover, 60000);

            if (!utf16) {
                // Completion with 5000 candidates: from asking to the list
                // being ready, and how long the main thread spends taking it in.
                std::vector<double> round, take;
                for (int i = 0; i < 15; ++i) {
                    const u64 a = now_ns();
                    lsp.complete(e.cursor(), 0);
                    for (;;) {
                        lsp.wait(1000);
                        const u64 p0 = now_ns();
                        const u32 got = lsp.poll();
                        const u64 p1 = now_ns();
                        if (got & Lsp::kCompletion) {
                            round.push_back(static_cast<double>(p1 - a) * 1e-6);
                            take.push_back(static_cast<double>(p1 - p0) * 1e-3);
                            break;
                        }
                        if (p1 - a > 10'000'000'000ull) break;
                    }
                }
                emit(file, "lsp_completion_items", "count", static_cast<double>(lsp.completions().size()));
                emit(file, "lsp_completion_roundtrip_ms", "ms", median(round));
                emit(file, "lsp_completion_take_in_main_thread_us", "us", median(take));
                // Narrowing the list as a letter is typed.
                const std::vector<CompletionItem>& items = lsp.completions();
                std::vector<std::pair<int, u32>> shown;
                for (const char* typed : {"i", "it", "item_1", "ivl"}) {
                    emit(file, (std::string("lsp_filter_5000_us_") + typed).c_str(), "us", 1e-3 * per_op(200, [&] {
                             shown.clear();
                             for (u32 i = 0; i < items.size(); ++i) {
                                 const int score = lsp_match(typed, items[i].filter);
                                 if (score >= 0) shown.push_back({score, i});
                             }
                             std::stable_sort(shown.begin(), shown.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
                             sink = shown.size();
                         }));
                }
            }
            e.unlisten(&lsp);
            lsp.detach();
        }

        // JSON: a completion list of 5000 items as a server would send it.
        json::Writer w;
        w.begin_object().member("jsonrpc", "2.0").member("id", 7).key("result").begin_object().member("isIncomplete", false).key("items").begin_array();
        for (int i = 0; i < 5000; ++i) {
            const std::string label = "item_" + std::to_string(i) + "_value";
            w.begin_object().member("label", label).member("kind", 6).member("detail", "std::vector<std::string>").member("sortText", label);
            w.key("textEdit").begin_object().key("range").begin_object();
            w.key("start").begin_object().member("line", 120).member("character", 4).end_object();
            w.key("end").begin_object().member("line", 120).member("character", 9).end_object();
            w.end_object().member("newText", label).end_object().end_object();
        }
        w.end_array().end_object().end_object();
        const std::string message = w.out;
        json::Doc doc;
        const double parse_ns = per_op(50, [&] {
            doc.parse(message);
            sink = doc.node_count();
        });
        emit(file, "json_parse_mb_per_s", "MB/s", static_cast<double>(message.size()) / (1u << 20) / (parse_ns * 1e-9));
        emit(file, "json_parse_completion_5000_ms", "ms", parse_ns * 1e-6);
        std::string out;
        const double quote_ns = per_op(50, [&] {
            out.clear();
            json::quote(out, message);
            sink = out.size();
        });
        emit(file, "json_quote_mb_per_s", "MB/s", static_cast<double>(message.size()) / (1u << 20) / (quote_ns * 1e-9));
    }
#endif

    if (want("load")) emit(file, "rss_peak_mb", "MB", rss_mb());
    return 0;
}

}  // namespace

// Stamps the wall clock and execs the program, so it can report the time
// from exec to its first frame (process creation and dynamic linking included).
int launch(int argc, char** argv) {
    static char stamp[64];
    timespec ts;
    std::timespec_get(&ts, TIME_UTC);
    std::snprintf(stamp, sizeof stamp, "%lld.%09ld", static_cast<long long>(ts.tv_sec), ts.tv_nsec);
    std::vector<char*> args(argv, argv + argc);
    static char flag[] = "--launch-wall";
    args.push_back(flag);
    args.push_back(stamp);
    args.push_back(nullptr);
    execv(args[0], args.data());
    std::perror(args[0]);
    return 1;
}

// The finder on a real folder, as the editor uses it: the folder read, then
// searches through the worker (text on several threads). Each search is
// timed from asking to the answer being there.
int tree(const char* dir, int argc, char** words) {
    Finder f;
    const u64 t0 = now_ns();
    f.index(dir);
    f.wait_idle();
    const double walk_ms = static_cast<double>(now_ns() - t0) * 1e-6;
    const std::shared_ptr<const FileIndex> ix = f.files();
    emit(dir, "tree_files", "count", static_cast<double>(ix->size()));
    emit(dir, "tree_read_ms", "ms", walk_ms);
    emit(dir, "tree_names_bytes", "bytes", static_cast<double>(ix->names.size()));
    // (The folder is read again before each search, so that each is a
    // first search: one typed onto the last is narrowed to what that found.)
    auto ask = [&](bool text, const char* typed) {
        double best = 1e30;
        for (int i = 0; i < 5; ++i) {
            f.cancel();  // (or the folder read again is searched again)
            f.index(dir);
            f.wait_idle();
            const u64 t = now_ns();
            text ? f.find_text(typed) : f.find_names(typed);
            f.wait_idle();
            best = std::min(best, static_cast<double>(now_ns() - t) * 1e-6);
        }
        const std::shared_ptr<const FindResults> r = f.results();
        emit(dir, (std::string(text ? "tree_text_ms " : "tree_names_ms ") + typed).c_str(), "ms", best);
        emit(dir, (std::string(text ? "tree_text_lines " : "tree_names_matched ") + typed).c_str(), "count", static_cast<double>(r->matched));
        // Typed a letter at a time, each search waited for: all of them together.
        double typing = 1e30;
        for (int i = 0; i < 3; ++i) {
            f.cancel();
            f.index(dir);
            f.wait_idle();
            const u64 t = now_ns();
            for (size_t k = 1; k <= std::strlen(typed); ++k) {
                const std::string part(typed, k);
                text ? f.find_text(part) : f.find_names(part);
                f.wait_idle();
            }
            typing = std::min(typing, static_cast<double>(now_ns() - t) * 1e-6);
        }
        emit(dir, (std::string(text ? "tree_text_typed_ms " : "tree_names_typed_ms ") + typed).c_str(), "ms", typing);
        // Then taken back a letter at a time (Backspace), down to two letters.
        double back = 1e30;
        for (int i = 0; i < 3; ++i) {
            f.cancel();
            f.index(dir);
            f.wait_idle();
            for (size_t k = 1; k <= std::strlen(typed); ++k) {
                text ? f.find_text(std::string(typed, k)) : f.find_names(std::string(typed, k));
                f.wait_idle();
            }
            const u64 t = now_ns();
            for (size_t k = std::strlen(typed); k-- > 2;) {
                text ? f.find_text(std::string(typed, k)) : f.find_names(std::string(typed, k));
                f.wait_idle();
            }
            back = std::min(back, static_cast<double>(now_ns() - t) * 1e-6);
        }
        emit(dir, (std::string(text ? "tree_text_backspaced_ms " : "tree_names_backspaced_ms ") + typed).c_str(), "ms", back);
    };
    ask(false, "stdio");
    ask(false, "nsstr h");
    ask(false, "zqzqzq");
    for (int i = 0; i < argc; ++i) ask(true, words[i]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "tree")) return tree(argv[2], argc - 3, argv + 3);
    if (argc >= 3 && !std::strcmp(argv[1], "launch")) return launch(argc - 2, argv + 2);
    if (argc >= 3 && !std::strcmp(argv[1], "gen")) return gen(argv[2]);
    if (argc >= 3 && !std::strcmp(argv[1], "run")) return run(argv[2], argc > 3 ? argv[3] : nullptr);
    std::fprintf(stderr, "usage: blit_bench gen DIR | run FILE [name] | tree DIR [words to search for] | launch PROGRAM [args]\n");
    return 2;
}
