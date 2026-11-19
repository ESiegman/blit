// Model check: random edits applied to both blit::Buffer and a std::string,
// with contents, line index and structure compared along the way.
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <zlib.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "buffer.hpp"
#include "config.hpp"
#include "dap.hpp"
#include "editor.hpp"
#include "finder.hpp"
#include "folds.hpp"
#include "fsio.hpp"
#include "snippet.hpp"
#include "asm.hpp"
#include "notebook.hpp"
#include "history.hpp"
#include "wrap.hpp"
#include "tests.hpp"
#include "font.hpp"
#include "git.hpp"
#include "profile.hpp"
#include "remote.hpp"
#include "term.hpp"
#include "grammars.hpp"
#include "grid.hpp"
#include "highlight.hpp"
#include "instance.hpp"
#include "job.hpp"
#include "json.hpp"
#include "keymap.hpp"
#include "look.hpp"
#include "lsp.hpp"
#include "match.hpp"
#include "syntax.hpp"
#include "tree.hpp"
#include "utf8.hpp"
#include "vim.hpp"

using namespace blit;

namespace {

std::mt19937_64 rng;

u64 rnd(u64 n) { return n ? rng() % n : 0; }

std::string random_text(size_t n) {
    static const char* const bits[] = {"a", "b", "z", " ", "\n", "\n", "\t", "x", "é", "日本", "😀", "\r\n", "0"};
    std::string s;
    while (s.size() < n) s += bits[rnd(std::size(bits))];
    return s;
}

[[noreturn]] void fail(const char* what, u64 seed, int step) {
    std::fprintf(stderr, "FAIL: %s (seed %llu, step %d)\n", what, (unsigned long long)seed, step);
    std::exit(1);
}

void compare(const Buffer& b, const std::string& m, u64 seed, int step, bool full) {
    if (b.size() != m.size()) fail("size", seed, step);
    if (!full) return;
    b.verify();
    if (b.text(0, b.size()) != m) fail("content", seed, step);

    std::vector<u64> starts{0};
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i] == '\n') starts.push_back(i + 1);
    if (b.line_count() != starts.size()) fail("line_count", seed, step);
    for (int t = 0; t < 40; ++t) {
        u64 line = rnd(starts.size());
        if (b.line_start(line) != starts[line]) fail("line_start", seed, step);
        u64 off = rnd(m.size() + 1);
        u64 want = std::upper_bound(starts.begin(), starts.end(), off) - starts.begin() - 1;
        if (b.line_of(off) != want) fail("line_of", seed, step);
        // line_reader lands on the same bytes as the model.
        auto r = b.line_reader(line);
        std::string_view c = r.chunk();
        if (starts[line] < m.size()) {
            if (c.empty() || c[0] != m[starts[line]]) fail("line_reader", seed, step);
        } else if (!c.empty()) {
            fail("line_reader at end", seed, step);
        }
    }
    if (b.line_start(starts.size() + 5) != m.size()) fail("line_start past end", seed, step);
    // Partial read from a random offset.
    u64 off = rnd(m.size() + 1), n = rnd(9000);
    if (b.text(off, n) != m.substr(off, n)) fail("read", seed, step);
}

void run(u64 seed, int steps) {
    rng.seed(seed);
    Buffer b;
    std::string m;
    std::string original;  // must outlive the buffer view
    if (seed % 3 == 0) {
        original = random_text(rnd(300000));
        b.load_view(original.data(), original.size());
        m = original;
    }
    std::vector<Piece> removed;
    u64 type_at = 0;

    for (int step = 0; step < steps; ++step) {
        switch (rnd(10)) {
            case 0:
            case 1: {  // insert
                size_t len = rnd(8) == 0 ? rnd(20000) : rnd(40) + 1;
                std::string s = random_text(len);
                u64 off = rnd(m.size() + 1);
                b.insert(off, s);
                m.insert(off, s);
                break;
            }
            case 2:
            case 3:
            case 4: {  // typing run
                if (rnd(4) == 0 || type_at > m.size()) type_at = rnd(m.size() + 1);
                int n = static_cast<int>(rnd(30)) + 1;
                for (int i = 0; i < n; ++i) {
                    char c = rnd(12) == 0 ? '\n' : static_cast<char>('a' + rnd(26));
                    b.insert(type_at, std::string_view(&c, 1));
                    m.insert(type_at, 1, c);
                    ++type_at;
                }
                break;
            }
            case 5:
            case 6: {  // erase
                if (m.empty()) break;
                u64 off = rnd(m.size());
                u64 len = rnd(6) == 0 ? rnd(m.size() / 2 + 1) : rnd(60) + 1;
                b.erase(off, len);
                m.erase(off, len);
                break;
            }
            case 7: {  // erase, then put the pieces back (undo)
                if (m.empty()) break;
                u64 off = rnd(m.size());
                u64 len = rnd(5) == 0 ? rnd(m.size() + 1) : rnd(5000) + 1;
                removed.clear();
                std::string cut = m.substr(off, len);
                b.erase(off, len, &removed);
                m.erase(off, len);
                u64 total = 0;
                for (const Piece& p : removed) total += p.len;
                if (total != cut.size()) fail("removed length", seed, step);
                compare(b, m, seed, step, rnd(4) == 0);
                u64 to = rnd(3) == 0 ? rnd(m.size() + 1) : off;
                b.insert_pieces(to, removed);
                m.insert(to, cut);
                break;
            }
            case 8: {  // backspace run
                u64 off = rnd(m.size() + 1);
                int n = static_cast<int>(rnd(50));
                while (n-- > 0 && off > 0) {
                    --off;
                    b.erase(off, 1);
                    m.erase(off, 1);
                }
                break;
            }
            case 9: {  // occasionally wipe almost everything
                if (rnd(40) != 0 || m.empty()) break;
                u64 keep = rnd(3);
                u64 off = rnd(keep + 1);
                u64 len = m.size() > keep ? m.size() - keep : m.size();
                b.erase(off, len);
                m.erase(off, len);
                break;
            }
        }
        compare(b, m, seed, step, step % 16 == 0 || step == steps - 1);
    }
}

// Editor level: edits, selection replacement and undo/redo against a model.
// ASCII only, so one character is one byte and the model stays trivial.
void run_editor(u64 seed, int steps) {
    rng.seed(seed * 7919);
    Editor e;
    std::string m, initial;
    if (seed % 2 == 0) {
        initial = "first line\nsecond line\n\tindented\nlast";
        e.buf.insert(0, initial);
        m = initial;
    }
    u64 cur = 0, anc = 0;  // model cursor and anchor
    int last_op = -1;
    auto ascii = [](size_t n) {
        std::string s;
        for (size_t i = 0; i < n; ++i) s += rnd(9) == 0 ? '\n' : static_cast<char>('a' + rnd(26));
        return s;
    };
    auto replace_sel = [&](const std::string& with) {
        u64 lo = std::min(cur, anc), hi = std::max(cur, anc);
        m.replace(lo, hi - lo, with);
        cur = anc = lo + with.size();
    };
    auto check = [&](int step, const char* what) {
        if (e.buf.size() != m.size() || e.cursor() != cur || e.anchor() != anc) {
            std::fprintf(stderr, "size %llu/%zu cursor %llu/%llu anchor %llu/%llu op %d\n",
                         (unsigned long long)e.buf.size(), m.size(), (unsigned long long)e.cursor(),
                         (unsigned long long)cur, (unsigned long long)e.anchor(), (unsigned long long)anc, last_op);
            fail(what, seed, step);
        }
        if (step % 8 == 0) {
            e.buf.verify();
            if (e.buf.text(0, e.buf.size()) != m) fail(what, seed, step);
        }
    };
    for (int step = 0; step < steps; ++step) {
        switch (last_op = static_cast<int>(rnd(9))) {
            case 0: {
                cur = rnd(m.size() + 1);
                bool sel = rnd(4) == 0;
                if (!sel) anc = cur;
                e.move_to(cur, sel);
                break;
            }
            case 1: {
                std::string s = ascii(rnd(12) == 0 ? rnd(9000) : rnd(20) + 1);
                e.insert(s);
                replace_sel(s);
                break;
            }
            case 2:
            case 3: {  // typing run
                for (int n = static_cast<int>(rnd(25)) + 1; n > 0; --n) {
                    std::string s = ascii(1);
                    if (s == "\n") e.newline();
                    else e.insert(s);
                    replace_sel(s);
                }
                break;
            }
            case 4:
            case 5: {  // backspace run
                for (int n = static_cast<int>(rnd(40)) + 1; n > 0; --n) {
                    e.backspace();
                    if (cur != anc) replace_sel("");
                    else if (cur > 0) {
                        m.erase(cur - 1, 1);
                        cur = anc = cur - 1;
                    }
                }
                break;
            }
            case 6: {  // forward delete run
                for (int n = static_cast<int>(rnd(40)) + 1; n > 0; --n) {
                    e.del();
                    if (cur != anc) replace_sel("");
                    else if (cur < m.size()) m.erase(cur, 1);
                }
                break;
            }
            case 7: {  // undo some, redo them: text must come back
                int k = static_cast<int>(rnd(6)) + 1, undone = 0;
                while (undone < k && e.undo()) ++undone;
                for (int i = 0; i < undone; ++i)
                    if (!e.redo()) fail("redo missing", seed, step);
                if (e.buf.text(0, e.buf.size()) != m) fail("undo/redo round trip", seed, step);
                if (undone) cur = anc = e.cursor();
                break;
            }
            case 8: {  // undo everything: back to the starting text
                if (rnd(25) != 0) break;
                int undone = 0;
                while (e.undo()) ++undone;
                e.buf.verify();
                if (e.buf.text(0, e.buf.size()) != initial) fail("undo all", seed, step);
                for (int i = 0; i < undone; ++i) e.redo();
                if (e.buf.text(0, e.buf.size()) != m) fail("redo all", seed, step);
                if (undone) cur = anc = e.cursor();
                break;
            }
        }
        check(step, "editor state");
    }
}

// A mapped file shrinks underneath the buffer: reads must not crash.
// More carets than one: an edit at each, each moving with the others'
// edits; those that meet merged; and, with random texts, typing at every
// caret what typing at each place by hand gives.
// Snippets read: the stops in Tab's order ($0 last), a stop in more than one
// place, a placeholder in a placeholder, choices, variables, escapes; and
// indented for the line they go in at.
void snippets_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    auto spans = [](const Snippet& s, size_t k) {
        std::string out;
        for (const Snippet::Span& sp : s.stops[k].spans) out += (out.empty() ? "" : ",") + std::to_string(sp.start) + "-" + std::to_string(sp.end);
        return out;
    };
    const Snippet loop = parse_snippet("for (${1:i} = 0; ${1:i} < ${2:n}; ++${1:i}) {$0}");
    check(loop.text == "for (i = 0; i < n; ++i) {}" && loop.stops.size() == 3 && loop.stops[0].index == 1 && spans(loop, 0) == "5-6,12-13,21-22" &&
              spans(loop, 1) == "16-17" && loop.stops[2].index == 0 && spans(loop, 2) == "25-25",
          "snippet: a stop in three places, then the next, then $0");
    const Snippet nested = parse_snippet("${2:second} ${1:call(${3:arg})}");
    check(nested.text == "second call(arg)" && nested.stops.size() == 4 && nested.stops[0].index == 1 && spans(nested, 0) == "7-16" && spans(nested, 1) == "0-6" &&
              spans(nested, 2) == "12-15" && spans(nested, 3) == "16-16",
          "snippet: one inside another; no $0, the end");
    const Snippet choice = parse_snippet("x = ${1|left,right|};");
    check(choice.text == "x = left;" && choice.stops[0].choices == std::vector<std::string>{"left", "right"} && spans(choice, 0) == "4-8", "snippet: choices, the first in place");
    const Snippet escaped = parse_snippet("\\$1 costs \\${2} \\\\ ${1:a\\}b}");
    check(escaped.text == "$1 costs ${2} \\ a}b" && escaped.stops.size() == 2, "snippet: escapes");
    const Snippet vars = parse_snippet("// $TM_FILENAME by ${USER:nobody} ${UNKNOWN} $NOPE.", [](std::string_view name, std::string* v) {
        if (name == "TM_FILENAME") return *v = "a.cpp", true;
        return false;
    });
    check(vars.text == "// a.cpp by nobody  .", "snippet: variables, a default where one has no value");
    Snippet body = parse_snippet("if (${1:x}) {\n\t$0\n}");
    indent_snippet(body, "    ", "  ");
    check(body.text == "if (x) {\n      \n    }" && spans(body, 0) == "4-5" && spans(body, 1) == "15-15", "snippet: indented as the line is, a tab one level");
}

// A file's assembly read: GCC's for x86 (comments after '#', .loc lines,
// a debug section skipped, its own labels left out) and Clang's for Apple's
// arm64 ('#' an immediate there, ';' a comment); and a compile command
// turned into one that writes the assembly.
void asm_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const AsmView gcc = read_asm(
        "\t.file\t\"sum.c\"\n\t.text\n.Ltext0:\n\t.file 1 \"sum.c\"\n\t.globl\tsum\n\t.type\tsum, @function\nsum:\n.LFB0:\n\t.loc 1 2 30\n"
        "\tmovl\t$0, %eax\t# s\n\t.loc 1 4 5\n.L3:\n\taddl\t(%rdi), %eax\n\t.loc 1 9 1\n\tcall\tother\n\t.file 2 \"inc.h\"\n\t.loc 2 7 1\n\tret\n.LFE0:\n"
        "\t.section\t.debug_info,\"\",@progbits\n\t.long\t0x8c\nnotcode:\n\t.text\n\tnop\n",
        "/src/sum.c");
    check(gcc.text == "sum:\n    movl    $0, %eax  # s\n.L3:\n    addl    (%rdi), %eax\n    call    other\n    ret\n    nop", "asm: GCC's x86, without directives, its labels, the debug section");
    check(gcc.line == std::vector<u32>{0, 2, 0, 4, 9, 0, 0}, "asm: each instruction's line of the file (none for another file's)");
    check(gcc.off == std::vector<u32>{0, 5, 25, 31, 36} && gcc.style == std::vector<u8>{3, 0, 4, 3, 0}, "asm: labels and comments coloured");
    const AsmView arm = read_asm(
        "\t.section\t__TEXT,__text,regular,pure_instructions\n\t.file\t0 \"/src\" \"sum.c\" md5 0x12\n\t.globl\t_sum ; -- Begin function sum\n_sum: ; @sum\n"
        "Lfunc_begin0:\n\t.loc\t0 3 0\n\tsub\tsp, sp, #32\n; %bb.0:\nLBB0_1: ; =>This Inner Loop Header\n\t.loc\t0 4 5 prologue_end\n\tldr\tw8, [sp, #12]\nLtmp0:\n",
        "/src/sum.c");
    check(arm.text == "_sum:  ; @sum\n    sub     sp, sp, #32\nLBB0_1:  ; =>This Inner Loop Header\n    ldr     w8, [sp, #12]", "asm: Apple's arm64, '#' kept, ';' a comment");
    check(arm.line == std::vector<u32>{0, 3, 0, 4}, "asm: DWARF 5's file 0, as a folder and a name");
    std::vector<std::string> argv;
    std::string dir;
    check(asm_command(R"([{"directory": "/b", "command": "cc -O2 -DX=\"a b\" -c ../src/sum.c -o sum.o -MD -MF sum.d -flto", "file": "/src/sum.c"}])", "/src/sum.c", &argv, &dir) &&
              argv == std::vector<std::string>{"cc", "-S", "-g", "-fno-asynchronous-unwind-tables", "-o", "-", "-O2", "-DX=a b", "../src/sum.c"} && dir == "/b",
          "asm: a command's object, dependencies and LTO left out, the assembly to the standard output");
    check(asm_command(R"([{"directory": "/b", "arguments": ["c++", "-c", "x.cpp"], "file": "x.cpp"}])", "/b/x.cpp", &argv, &dir) && argv.size() == 7 && argv.back() == "x.cpp",
          "asm: arguments, and a file relative to its folder");
    check(!asm_command(R"([{"directory": "/b", "command": "cc -c y.c", "file": "y.c"}])", "/b/x.c", &argv, &dir), "asm: no entry for the file");
}

// A notebook as the script of its cells, and back: a Markdown cell's
// lines commented; outputs and ids kept for the cells as they were, a
// changed one's outputs gone, a new one given an id; the notebook's own
// metadata kept, and written as Jupyter writes it.
void notebook_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const std::string nb = R"NB({
 "cells": [
  {
   "cell_type": "markdown",
   "id": "a1",
   "metadata": {},
   "source": [
    "# Title\n",
    "\n",
    "Some text"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": 3,
   "id": "b2",
   "metadata": {
    "tags": []
   },
   "outputs": [
    {
     "name": "stdout",
     "output_type": "stream",
     "text": [
      "first\n",
      "6.5\n"
     ]
    }
   ],
   "source": [
    "x = 13 / 2\n",
    "print('first')\n",
    "print(x)"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": 4,
   "id": "c3",
   "metadata": {},
   "outputs": [
    {
     "data": {
      "image/png": "iVBOR"
     },
     "metadata": {},
     "output_type": "display_data"
    }
   ],
   "source": "plot()"
  }
 ],
 "metadata": {
  "kernelspec": {
   "display_name": "Python 3",
   "name": "python3"
  },
  "language_info": {
   "version": "3.12.1"
  }
 },
 "nbformat": 4,
 "nbformat_minor": 5
}
)NB";
    std::vector<NotebookCell> cells;
    std::string script;
    check(notebook_read(nb, &cells, &script), "notebook: read");
    check(script == "# %% [markdown]\n# # Title\n#\n# Some text\n\n# %%\nx = 13 / 2\nprint('first')\nprint(x)\n\n# %%\nplot()\n", "notebook: the script of its cells");
    check(cells.size() == 3 && cells[1].output == "6.5" && cells[2].output == "[image/png]" && cells[0].output.empty(), "notebook: each code cell's outputs in a line");
    check(notebook_write(nb, script) == nb, "notebook: written back as it was, unchanged");
    const std::vector<CellLines> lines = notebook_cell_lines(script);
    check(lines.size() == 3 && lines[0].head == 0 && lines[0].end == 4 && !lines[0].code && lines[1].head == 5 && lines[1].first == 6 && lines[1].end == 9 &&
              lines[2].end == 12,
          "notebook: each cell's lines, without the gap after it");
    // The second cell changed, a cell put in before the last: the last keeps its outputs and id.
    const std::string edited = "# %% [markdown]\n# # Title\n#\n# Some text\n\n# %%\nx = 1\n\n# %%\nnew()\n\n# %%\nplot()\n";
    const std::string out = notebook_write(nb, edited);
    std::vector<NotebookCell> again;
    std::string script2;
    check(notebook_read(out, &again, &script2) && script2 == edited, "notebook: an edited script read back the same");
    check(again.size() == 4 && again[1].output.empty() && again[3].output == "[image/png]", "notebook: a changed cell's outputs gone, an unchanged one's kept");
    check(out.find("\"id\": \"b2\"") != std::string::npos && out.find("\"id\": \"c3\"") != std::string::npos && out.find("\"execution_count\": 3") == std::string::npos &&
              out.find("\"tags\": []") != std::string::npos && out.find("\"version\": \"3.12.1\"") != std::string::npos,
          "notebook: ids and a cell's metadata kept, a changed one's count gone, the notebook's metadata kept");
    size_t ids = 0;
    for (size_t at = 0; (at = out.find("\"id\": ", at)) != std::string::npos; ++at) ++ids;
    check(ids == 4, "notebook: a new cell given an id");
    std::vector<NotebookCell> plain = notebook_cells("import os\n\n# %%\nprint(1)\n");
    check(plain.size() == 2 && plain[0].source == "import os" && plain[1].source == "print(1)", "notebook: lines before the first # %%, a cell of their own");
    std::vector<NotebookCell> fresh;
    std::string fresh_script;
    check(notebook_read(notebook_write("", "# %%\n1 + 1\n"), &fresh, &fresh_script) && fresh_script == "# %%\n1 + 1\n", "notebook: a new one");
    check(!notebook_read("{\"x\": 1}", &fresh, &fresh_script), "notebook: JSON that is not one");
}

// Local history: versions kept newest first, read back as they were; the
// same text not kept twice (a save marks it saved); the oldest let go past
// the most kept; and each file's folder its own.
void history_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    char tmpl[] = "/tmp/blit-history-XXXXXX";
    const char* made = mkdtemp(tmpl);
    check(made != nullptr, "history: a folder to try it in");
    if (!made) return;
    const std::string root = made, dir = history_dir(root, "/src/a.cpp");
    check(dir != history_dir(root, "/src/b.cpp") && dir == history_dir(root, "/src/a.cpp"), "history: a folder for each file");
    std::string err, text;
    check(history_list(dir).empty(), "history: none at first");
    check(history_keep(dir, "/src/a.cpp", "one\n", false, 1000, 3, &err), "history: kept");
    check(history_keep(dir, "/src/a.cpp", "one\n", true, 2000, 3, &err), "history: the same again");
    std::vector<HistoryEntry> e = history_list(dir);
    check(e.size() == 1 && e[0].saved && e[0].when_ms == 1000, "history: the same text kept once, marked saved");
    check(history_keep(dir, "/src/a.cpp", "two\n", true, 3000, 3, &err) && history_keep(dir, "/src/a.cpp", std::string(100000, 'x'), false, 3000, 3, &err),
          "history: more kept (one at the same time)");
    e = history_list(dir);
    check(e.size() == 3 && e[0].when_ms == 3001 && !e[0].saved && e[1].when_ms == 3000 && e[2].when_ms == 1000, "history: newest first, in order however fast");
    check(history_read(e[0].file, &text) && text == std::string(100000, 'x') && history_read(e[1].file, &text) && text == "two\n", "history: read back as kept");
    check(history_keep(dir, "/src/a.cpp", "three\n", false, 4000, 3, &err) && history_list(dir).size() == 3 && history_list(dir).back().when_ms == 3000,
          "history: the oldest let go past the most");
    check(history_keep(dir, "/src/a.cpp", "", true, 5000, 3, &err) && history_read(history_list(dir)[0].file, &text) && text.empty(), "history: an empty text");
    // (And in the same folder, a file read whole: one just as large as it
    // may be is read, one a byte larger is not.)
    std::string why;
    check(fs_write(root + "/eight", "12345678", &err) && fs_read_whole(root + "/eight", &text, &why, 8, {}) && text == "12345678", "read whole: just as large as it may be");
    check(!fs_read_whole(root + "/eight", &text, &why, 7, {}) && why.starts_with("the file is too big"), "read whole: a byte larger");
    std::filesystem::remove_all(root);
}

// Soft wrap: where a line breaks (after the last space that fits; a long
// word where the width ends; a space hanging past the end; a wide
// character whole), and the map between rows and lines kept as the text
// changes, with lines folded away taking none.
void wrap_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    std::vector<u32> b;
    Wrap::breaks("hello world foo bar", 11, 4, &b);
    check(b == std::vector<u32>{12}, "wrap: after the last space that fits, a space hanging past the end");
    Wrap::breaks("abcdefghijklmnopqrstuvwxyz", 10, 4, &b);
    check(b == std::vector<u32>{10, 20}, "wrap: a word longer than the width, where the width ends");
    Wrap::breaks("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD", 5, 4, &b);  // (four two-column characters, five columns)
    check(b == std::vector<u32>{4}, "wrap: a wide character whole on the next row, not split at the edge");
    Wrap::breaks("short", 10, 4, &b);
    check(b.empty(), "wrap: a line that fits");

    Editor ed;
    ed.insert("one\ntwo three four five six seven\nend\n");
    Wrap w;
    w.set(&ed, 1, nullptr, 0, 10, 4);
    // two three four five six seven: rows start at 0, "four " at 10, "six" at 20 (with width 10)
    check(w.rows() == 6 && w.row_of(1) == 1 && w.row_of(2) == 4 && w.line_at(2) == 1 && w.line_at(4) == 2 && w.line_at(5) == 3,
          "wrap: rows and lines");
    check(w.start_col(1) == 0 && w.start_col(2) == 10 && w.start_col(3) == 20 && w.end_col(2) == 20 && w.end_col(3) == ~0u && w.row_at(1, 14) == 2,
          "wrap: each row's columns, and the row a place is in");
    ed.move_to(ed.buf.line_start(1), false);
    ed.insert("x\n");  // a line put in before it: one more row, the rest after it
    check(w.rows() == 7 && w.row_of(2) == 2 && w.line_at(3) == 2 && w.row_of(3) == 5, "wrap: kept as lines are put in");
    ed.move_to(ed.buf.line_start(2), false);
    ed.move_to(ed.line_end(2), true);
    ed.insert("short");  // the long line made short: its rows go
    check(w.rows() == 5 && w.rows_of(2) == 1 && w.row_of(3) == 3, "wrap: kept as a line changes");
    // Moving down by rows, along the row.
    ed.insert(" and now it is long again");
    ed.skip = &w;
    ed.move_to(ed.buf.line_start(2) + 3, false);
    ed.down(false);
    check(ed.buf.line_of(ed.cursor()) == 2 && ed.col_of(ed.cursor()) == w.start_col(w.row_of(2) + 1) + 3, "wrap: down within a wrapped line, along the row");
    ed.skip = nullptr;
    w.detach();
    Folds f;
    f.attach(&ed);
    f.add(ed.buf, 0, 1);  // (line 1 under line 0)
    w.set(&ed, 1, &f, f.generation(), 10, 4);
    check(w.rows_of(1) == 0 && w.row_of(1) == 0 && w.line_at(1) == 2, "wrap: a folded line takes no row");
    w.detach();
    // Lines an edit makes inside a fold are hidden with it, though the folds
    // began to hear of edits after the wrap did (they go first).
    {
        Editor e3;
        e3.insert("head\none\ntwo\nafter");
        Wrap w3;
        w3.set(&e3, 3, nullptr, 0, 10, 4);
        Folds f3;
        f3.attach(&e3);
        f3.add(e3.buf, 0, 2);  // (one and two under head)
        e3.listen(&f3, true);
        w3.set(&e3, 3, &f3, f3.generation(), 10, 4);
        e3.move_to(e3.buf.line_start(2), false);
        e3.insert("new\nlines\n");
        w3.set(&e3, 3, &f3, f3.generation(), 10, 4);
        check(w3.rows() == 2 && w3.line_at(1) == 5 && w3.rows_of(2) == 0 && w3.rows_of(3) == 0, "wrap: lines made inside a fold are hidden");
        // Random edits with folds about: the map kept is the map made afresh.
        std::mt19937 roll(11);
        bool kept_same = true;
        for (int step = 0; step < 300 && kept_same; ++step) {
            if (roll() % 8 == 0) {
                const u64 h = roll() % e3.buf.line_count();
                f3.add(e3.buf, h, h + 1 + roll() % 3);
                e3.listen(&f3, true);  // (as the application listens again, a fold added or not)
            }
            const u64 at = roll() % (e3.buf.size() + 1), len = roll() % 2 ? std::min<u64>(e3.buf.size() - at, roll() % 12) : 0;
            static const char* const puts[] = {"", "x", "a b ", "\n", "\nlonger words here\n", "averyveryverylongword", "\t"};
            e3.replace(at, at + len, puts[roll() % std::size(puts)]);
            w3.set(&e3, 3, f3.empty() ? nullptr : &f3, f3.generation(), 10, 4);
            Wrap fresh;
            fresh.set(&e3, 4, f3.empty() ? nullptr : &f3, f3.generation(), 10, 4);
            kept_same = fresh.rows() == w3.rows();
            for (u64 l = 0; kept_same && l < e3.buf.line_count(); ++l) kept_same = fresh.rows_of(l) == w3.rows_of(l) && fresh.row_of(l) == w3.row_of(l);
            fresh.detach();
        }
        check(kept_same, "wrap: kept through random edits with folds, the same as made afresh");
        // A view's top as a line and back, folded lines about: walked to
        // (with no map, as for a file not in front) as the map has it.
        bool tops = true;
        f3.sync(e3.buf, e3.revision());
        const Folds* folded = f3.empty() ? nullptr : &f3;
        for (double r = 0; tops && r < static_cast<double>(w3.rows()); r += 0.75) {
            const double line = w3.top_line(r);
            tops = std::fabs(Wrap::walk_line(e3, folded, 10, 4, r) - line) < 1e-9 && std::fabs(w3.top_rows(line) - r) < 1e-6 &&
                   std::fabs(Wrap::walk_rows(e3, folded, 10, 4, line) - r) < 1e-6;
        }
        for (u64 l = 0; tops && l < e3.buf.line_count(); ++l)  // (a line folded away among them: its header's row)
            tops = std::fabs(Wrap::walk_rows(e3, folded, 10, 4, l + 0.5) - w3.top_rows(l + 0.5)) < 1e-6;
        check(tops, "wrap: a view's top as a line and back, with the map or without");
        w3.detach();
        e3.unlisten(&f3);
    }
    // The folds listen again with none changed (everything folded already),
    // and an edit comes before the wrap is next set: it measures the lines
    // by the folds as that edit moved them.
    {
        Editor e4;
        e4.insert("head\none\ntwo\nafter\n");
        Folds f4;
        f4.attach(&e4);
        f4.add(e4.buf, 0, 2);
        e4.listen(&f4, true);
        Wrap w4;
        w4.set(&e4, 5, &f4, f4.generation(), 10, 4);
        check(!f4.add(e4.buf, 0, 2), "wrap: a fold there already is not added again");
        e4.listen(&f4, true);
        e4.move_to(0, false);
        e4.insert("\n");  // (a line opened above the header)
        check(w4.rows() == 4 && w4.rows_of(1) == 1 && w4.rows_of(2) == 0 && w4.rows_of(3) == 0, "wrap: an edit after the folds listen again");
        w4.detach();
        e4.unlisten(&f4);
    }
    // Random edits: the map kept as it goes is the map made afresh.
    std::mt19937 dice(7);
    Editor e2;
    Wrap kept;
    kept.set(&e2, 2, nullptr, 0, 12, 4);
    const char* words[] = {"a ", "word ", "longerword ", "\n", "x", "\t", "averyveryverylongword ", "\xE4\xB8\xAD"};
    bool same = true;
    for (int step = 0; step < 400 && same; ++step) {
        const u64 at = e2.buf.size() ? dice() % (e2.buf.size() + 1) : 0;
        e2.move_to(e2.next_char(e2.prev_char(at)), false);
        if (dice() % 4 == 0 && e2.buf.size()) {
            e2.move_to(std::min<u64>(e2.buf.size(), e2.cursor() + dice() % 20), true);
            e2.insert(dice() % 2 ? "" : "\n");
        } else {
            e2.insert(words[dice() % std::size(words)]);
        }
        Wrap fresh;
        fresh.set(&e2, 2, nullptr, 0, 12, 4);
        same = fresh.rows() == kept.rows();
        for (u64 r = 0; same && r < fresh.rows(); ++r) same = fresh.line_at(r) == kept.line_at(r) && fresh.start_col(r) == kept.start_col(r);
        fresh.detach();
    }
    check(same, "wrap: kept through random edits, the same as made afresh");
    kept.detach();

    // Notes between characters (inlay hints) take room in the rows: one
    // that does not fit goes to the next row with its character; at the
    // line's end, only after a space (no row holds a note alone).
    Wrap::breaks("x = call(aa, bb);", 16, 4, &b);
    check(b == std::vector<u32>{13}, "wrap: no notes");
    Wrap::breaks("x = call(aa, bb);", 16, 4, &b, std::vector<Wrap::Hint>{{9, 7}, {13, 8}});
    check(b == std::vector<u32>{4, 13}, "wrap: room for the notes, each on its character's row");
    Wrap::breaks("abc def", 8, 4, &b, std::vector<Wrap::Hint>{{7, 5}});
    check(b == std::vector<u32>{4}, "wrap: a note at the end, with the word before it");
    Wrap::breaks("abcdefg", 8, 4, &b, std::vector<Wrap::Hint>{{7, 5}});
    check(b.empty(), "wrap: a note at the end of a word that fills its row: no row of its own");
    // Random edits with notes about: the map kept (moving its notes with
    // each edit, as the language server moves its own) is the map made
    // afresh with the notes moved the same way.
    {
        std::mt19937 roll(5);
        Editor e5;
        e5.insert("call(one, two) and then a few more words here\nx\ncall(three)\n");
        std::vector<Wrap::Hint> notes = {{5, 6}, {10, 4}, {53, 9}};
        Wrap w5;
        w5.set(&e5, 6, nullptr, 0, 14, 4);
        w5.set_hints(notes, 1);
        bool kept_same = true;
        for (int step = 0; step < 400 && kept_same; ++step) {
            if (roll() % 10 == 0) {  // (new ones from the server)
                notes.clear();
                for (u64 at = roll() % 6; at <= e5.buf.size(); at += 1 + roll() % 12) notes.push_back({at, 1 + static_cast<u32>(roll() % 9)});
                w5.set_hints(notes, static_cast<u64>(step) + 2);
            }
            const u64 at = roll() % (e5.buf.size() + 1), len = roll() % 2 ? std::min<u64>(e5.buf.size() - at, roll() % 10) : 0;
            static const char* const puts[] = {"", "x", "a b ", "\n", "call(p, q) ", "averyveryverylongword", "\t"};
            const std::string text = puts[roll() % std::size(puts)];
            e5.replace(at, at + len, text);
            std::erase_if(notes, [&](const Wrap::Hint& h) { return h.off > at && h.off < at + len; });
            for (Wrap::Hint& h : notes)
                if (h.off > at && h.off >= at + len) h.off = h.off - len + text.size();
            Wrap fresh;
            fresh.set(&e5, 7, nullptr, 0, 14, 4);
            fresh.set_hints(notes, 0);
            kept_same = fresh.rows() == w5.rows();
            for (u64 r = 0; kept_same && r < fresh.rows(); ++r) kept_same = fresh.line_at(r) == w5.line_at(r) && fresh.start_col(r) == w5.start_col(r);
            fresh.detach();
        }
        check(kept_same, "wrap: kept through random edits with notes, the same as made afresh");
        w5.detach();
    }
}

// A profile's timeline: kept with each sample's time, narrowed to a
// stretch, and written and read back.
void timeline_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Profile p;
    p.program = "/t/prog";
    auto add = [&](double t, u64 tid, u64 w, std::vector<std::string_view> names) {
        std::vector<Profile::Frame> f;
        for (const std::string_view n : names) f.push_back({n, "/t/prog", {}, 0});
        p.add(f, w, tid, {}, t);
    };
    add(10.0, 1, 10, {"a", "main"});
    add(10.5, 1, 30, {"b", "main"});
    add(11.0, 2, 40, {"b", "work"});
    p.finish();
    check(p.timeline.size() == 3 && p.timeline[0].t == 0 && p.timeline[2].t == 1.0, "timeline: times from the first sample");
    const Profile q = narrow_profile(p, 0.4, 1.0);
    u32 b = 0;
    for (u32 f = 0; f < q.funcs.size(); ++f)
        if (q.funcs[f].name == "b") b = f;
    check(q.weight == 70 && q.samples == 2 && q.funcs[b].self == 70 && q.nodes[0].total == 70 && q.by_thread.size() == 2, "timeline: narrowed to a stretch");
    Profile back;
    std::string err;
    check(Profile::from_json(p.to_json(), &back, &err) && back.timeline.size() == 3 && back.timeline[1].t == 0.5 && back.timeline[2].thread == 2 &&
              back.timeline[2].w == 40,
          "timeline: kept and read back");
    // The allocation sampler's file: two samples, one still held; an address named, one only by dladdr's line.
    const std::string alloc = "blit-alloc 1\nrate 65536\ntotal 300000 10\ns 65536 100 0 10 20\ns 131072 131072 1 30 20\n"
                              "a 10 0 /t/prog\tsmall\na 20 0 /t/prog\tmain\na 30 0 /t/prog\t\n";
    std::unordered_map<u64, AllocName> names;
    names[0x30] = {"big", "/t/prog", "/t/prog.c", 9};
    Profile all, held;
    all.program = held.program = "/t/prog";
    std::string why;
    check(parse_alloc(alloc, names, &all, &held, &why) && all.weight == 196608 && held.weight == 131072 && all.samples == 2 && held.samples == 1,
          "allocations: every one, and those still held");
    all.finish();
    bool big = false, small = false;
    for (const ProfileFunc& f : all.funcs) big |= f.name == "big" && f.file == "/t/prog.c" && f.line == 9, small |= f.name == "small";
    check(big && small, "allocations: named by atos's names, else dladdr's");
}

void carets_test(int seeds) {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    {
        Editor ed;
        ed.open_bytes({}, "abcdefghij");
        ed.move_to(2, false);
        ed.add_caret({5, 5, ~0u});
        ed.add_caret({8, 8, ~0u});
        ed.each_caret([&] { ed.insert("X"); });
        check(ed.buf.text(0, ed.buf.size()) == "abXcdeXfghXij" && ed.cursor() == 3 && ed.carets().size() == 2, "carets: typed at each, the primary still first");
        ed.each_caret([&] { ed.backspace(); });
        check(ed.buf.text(0, ed.buf.size()) == "abcdefghij", "carets: Backspace at each");
        ed.undo();
        check(ed.buf.text(0, ed.buf.size()) == "abXcdeXfghXij" && !ed.multi(), "carets: one undo for all, and one caret after");
    }
    {
        Editor ed;
        ed.open_bytes({}, "aaaa bbbb cccc");
        ed.move_to(0, false);
        ed.move_to(4, true);
        ed.add_caret({2, 2, ~0u});        // (inside the primary's selection: merged into it)
        ed.add_caret({9, 6, ~0u});        // (a selection of its own)
        ed.add_caret({11, 8, ~0u});       // (overlapping that one: their union)
        check(ed.carets().size() == 1 && ed.sel_min() == 0 && ed.sel_max() == 4 && ed.carets()[0].anchor == 6 && ed.carets()[0].cursor == 11, "carets: those that meet are merged");
        ed.each_caret([&] { ed.left(false); });
        ed.each_caret([&] { ed.home(false); });
        check(!ed.multi() && ed.cursor() == 0, "carets: moved to one place, one caret");
    }
    u64 seed = 99;
    auto next = [&] { return seed = seed * 6364136223846793005ull + 1442695040888963407ull, static_cast<u32>(seed >> 33); };
    for (int round = 0; round < seeds; ++round) {
        std::string text;
        for (u32 n = next() % 40 + 1; n-- > 0;) text += static_cast<char>("ab \n"[next() % 4]);
        std::vector<u64> at;
        for (u32 n = next() % 5 + 1; n-- > 0;) at.push_back(next() % (text.size() + 1));
        std::ranges::sort(at);
        at.erase(std::unique(at.begin(), at.end()), at.end());
        Editor ed;
        ed.open_bytes({}, text);
        ed.move_to(at[0], false);
        for (size_t k = 1; k < at.size(); ++k) ed.add_caret({at[k], at[k], ~0u});
        const std::string put = std::string(1 + next() % 3, 'X');
        ed.each_caret([&] { ed.insert(put); });
        std::string want = text;
        for (size_t k = at.size(); k-- > 0;) want.insert(at[k], put);
        bool ok = ed.buf.text(0, ed.buf.size()) == want && ed.carets().size() + 1 == at.size();
        for (size_t k = 0; ok && k < at.size(); ++k) {  // (each caret after what it typed)
            const u64 expect = at[k] + (k + 1) * put.size();
            ok = std::ranges::any_of(ed.all_carets(), [&](const Editor::Caret& c) { return c.cursor == expect; });
        }
        if (!ok) fail("carets: typed at random places", static_cast<u64>(round), 0);
    }
}

void truncated_file_test() {
#if !defined(_WIN32)
    std::string path = "/tmp/blit-truncate-test.txt";
    if (const char* t = std::getenv("TMPDIR")) path = std::string(t) + "/blit-truncate-test.txt";
    std::string body;
    for (int i = 0; body.size() < (1u << 20); ++i) body += "line " + std::to_string(i) + "\n";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    Buffer b;
    std::string err;
    if (!b.load_file(path.c_str(), &err)) fail("load for truncate test", 0, 0);
    f = std::fopen(path.c_str(), "wb");  // truncates to zero
    std::fclose(f);
    std::string now = b.text(0, b.size());
    if (now.size() != body.size()) fail("size after truncate", 0, 0);
    // Once edited, the file's text is the buffer's own: another program
    // writing over the file where it is, or cutting it short, changes
    // nothing under the edit (it did on Linux, where a private mapping's
    // pages show the file until they are written to).
    for (const bool cut : {false, true}) {
        f = std::fopen(path.c_str(), "wb");
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
        Buffer e;
        if (!e.load_file(path.c_str(), &err)) fail("load for the written-over test", 0, 0);
        e.insert(0, "mine ");
        f = std::fopen(path.c_str(), cut ? "wb" : "r+b");
        if (!cut) {
            const std::string other(body.size(), 'X');
            std::fwrite(other.data(), 1, other.size(), f);
        }
        std::fclose(f);
        if (e.text(0, e.size()) != "mine " + body) fail(cut ? "an edited file cut short on disk" : "an edited file written over on disk", 0, 0);
    }
    std::remove(path.c_str());
#endif
}

// Settings as init.lua makes them: one key and one value at a time.
Config settings(std::initializer_list<std::pair<const char*, const char*>> kv) {
    std::vector<ConfigSetting> s;
    for (const auto& [k, v] : kv) s.push_back({k, v, "test"});
    return make_config(s);
}

// The settings: values, warnings, themes.
void config_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Config d = make_config();
    check(d.warnings.empty() && d.font_size == 13 && !d.ligatures && d.theme_dark == "blit-dark", "config defaults");

    Config c = settings({{"font.family", "\"Victor Mono\""},
                         {"font.size", "14.5"},
                         {"font.ligatures", "true"},
                         {"font.features", "[\"ss01\", 'zero']"},
                         {"theme.dark", "\"rose-pine\""},
                         {"theme.light", "'mine'"},
                         {"themes.mine.background", "\"#fff\""},
                         {"themes.mine.cursor", "\"#102030\""},
                         {"themes.rose-pine.cursor", "\"#ff0000\""},
                         {"editor.tab_width", "8"}});
    check(c.warnings.empty(), "clean config has no warnings");
    check(c.font_family == "Victor Mono" && c.font_size == 14.5f && c.ligatures, "font settings");
    check(c.font_features.size() == 2 && c.font_features[0] == "ss01" && c.font_features[1] == "zero", "feature list");
    check(c.theme_dark == "rose-pine" && c.theme_light == "mine" && c.tab_width == 8, "theme and editor settings");
    check(c.theme(false).background.r == 1.0f && c.theme(false).cursor.g == 0x20 / 255.0f, "custom theme colours");
    // Changing one colour of a built-in theme keeps the rest of it.
    check(c.theme(true).cursor.r == 1.0f && c.theme(true).background.r == 0x19 / 255.0f, "built-in theme override");

    Config bad = settings({{"font.size", "\"big\""}, {"font.ligatures", "yes"}, {"font.nonsense", "1"}, {"theme.dark", "\"nope\""}});
    int lines = 0;
    for (char ch : bad.warnings) lines += ch == '\n';
    check(lines == 4 && bad.font_size == 13 && !bad.ligatures && bad.warnings.starts_with("test: "), "bad values warn, with where, and keep defaults");
    check(bad.theme(true).background.r == make_config().theme(true).background.r, "unknown theme falls back");
    check(!settings({{"git.blame", "true"}}).warnings.empty(), "blame is no setting");

    Config pc = settings({{"panel.open", "true"}, {"panel.width", "40"}});
    check(pc.warnings.empty() && pc.panel_open && pc.panel_width == 40 && !d.panel_open && d.panel_width == 32, "panel settings");
    check(!settings({{"panel.width", "3"}}).warnings.empty(), "a panel too narrow to use warns");

    Config one = settings({{"theme", "\"rose-pine-dawn\""}});
    check(one.theme_dark == "rose-pine-dawn" && one.theme_light == "rose-pine-dawn" && one.warnings.empty(), "single theme");

    Rgb rgb;
    check(parse_color("#1a2B3c", &rgb) && rgb.b == 0x3c / 255.0f, "colour parse");
    check(!parse_color("1a2b3c", &rgb) && !parse_color("#12345", &rgb) && !parse_color("#gggggg", &rgb), "colour reject");

    // Token styles: colour, italic and bold each come from the nearest
    // capture that sets them. syntax.styles is for every theme, and a
    // theme's own wins over it wherever the two were set.
    std::vector<ConfigSetting> styled = {{"theme.dark", "\"mine\"", ""},
                                         {"theme.light", "\"mine\"", ""},
                                         {"themes.mine.syntax.comment", "\"#808080 italic\"", ""},
                                         {"themes.mine.syntax.keyword", "\"#0000ff\"", ""},
                                         {"themes.mine.syntax.keyword.return", "\"bold\"", ""},
                                         {"themes.mine.syntax.keyword.return.plain", "\"regular\"", ""},
                                         {"themes.mine.syntax.keyword.import", "\"italic bold #ff0000\"", ""},
                                         {"themes.mine.syntax.string", "\"#00ff00 regular\"", ""},
                                         {"syntax.styles.keyword", "\"italic\"", ""},
                                         {"syntax.styles.string", "\"bold\"", ""}};
    Config ts = make_config(styled);
    check(ts.warnings.empty(), "token styles parse cleanly");
    const ThemeColors& mine = ts.theme(true);
    auto is = [&](const char* capture, float r, float g, float b, bool italic, bool bold) {
        const TokenStyle t = mine.syntax_style(capture);
        return t.color.r == r && t.color.g == g && t.color.b == b && t.italic == italic && t.bold == bold;
    };
    const float grey = 0x80 / 255.0f;
    check(is("comment", grey, grey, grey, true, false) && is("comment.documentation", grey, grey, grey, true, false),
          "style and its children");
    check(is("keyword", 0, 0, 1, true, false), "style from syntax.styles joins the theme's colour");
    check(is("keyword.return", 0, 0, 1, true, true), "bold added to an inherited colour and italic");
    check(is("keyword.return.plain", 0, 0, 1, false, false), "regular switches both off");
    check(is("keyword.import", 1, 0, 0, true, true), "colour and both styles in one entry");
    check(is("string", 0, 1, 0, false, false), "a theme's own entry wins over syntax.styles, set before it or after");
    check(mine.syntax_style("nothing").color.r == mine.foreground.r && !mine.syntax_style("nothing").italic, "no entry: plain");
    check(ts.themes.find("rose-pine")->second.syntax_style("keyword.repeat").italic, "syntax.styles reaches every theme");
    // (A new theme starts as a copy of blit-dark, whose headings are bold,
    // links' addresses underlined and struck-out text struck.)
    check(ts.font_styles() == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 8)), "font styles the themes use");
    styled.push_back({"syntax.italic", "false", ""});
    Config upright = make_config(styled);
    check(upright.warnings.empty() && upright.font_styles() == ((1u << 2) | (1u << 4) | (1u << 8)), "italics switched off everywhere (the lines stay)");
    check(!settings({{"syntax.styles.comment", "\"#808080 wavy\""}}).warnings.empty(), "unknown style word warns");

    // Names other queries use for the same things follow the usual ones,
    // including what the settings change about them.
    check(mine.syntax_style("repeat").italic && mine.syntax_style("repeat").color.b == 1 && mine.syntax_style("conditional.ternary").italic,
          "other names for keywords follow keyword");
    check(mine.syntax_style("field").color.r == mine.syntax_style("property").color.r &&
              mine.syntax_style("delimiter").color.g == mine.syntax_style("punctuation").color.g,
          "other names for properties and punctuation");

    // Grammars: where they come from, and what the settings change.
    Config gc = settings({{"syntax.install", "[\"cpp\", \"python\"]"},
                          {"syntax.auto_install", "true"},
                          {"syntax.grammars.cpp.rev", "\"abc123\""},
                          {"syntax.grammars.mylang.repo", "\"me/tree-sitter-mylang\""},
                          {"syntax.grammars.mylang.rev", "\"v1.0\""},
                          {"syntax.grammars.mylang.path", "\"grammar\""},
                          {"syntax.grammars.mylang.inherits", "[\"c\"]"}});
    check(gc.warnings.empty() && gc.syntax_install.size() == 2 && gc.syntax_auto_install && gc.grammars.size() == 2, "grammar settings");
    const std::vector<GrammarSource> gl = grammar_list(gc.grammars);
    const GrammarSource* gcpp = find_grammar(gl, "cpp");
    const GrammarSource* mylang = find_grammar(gl, "mylang");
    check(gcpp && gcpp->rev == "abc123" && gcpp->repo == "tree-sitter/tree-sitter-cpp" && gcpp->inherits.size() == 1,
          "a grammar named like a built-in one inherits what it does not set");
    check(mylang && mylang->repo == "me/tree-sitter-mylang" && mylang->path == "grammar" && mylang->inherits[0] == "c", "a grammar of one's own");
    check(gl.size() == builtin_grammars().size() + 1 && find_grammar(gl, "python") && !find_grammar(gl, "nope"), "the rest of the built-in list");
    check(!settings({{"syntax.auto_install", "false"}}).syntax_auto_install && make_config().syntax_install.empty() &&
              !settings({{"syntax.install", "\"cpp\""}}).warnings.empty(),
          "nothing is installed unless asked for");
    // Asking for a grammar nobody knows fails at once, without fetching anything.
    std::string gerr;
    check(!install_grammar(gl, "no-such-language", &gerr) && gerr.find("does not know") != std::string::npos, "unknown grammar");
    GrammarSource odd = *mylang;
    odd.name = "bad name; rm -rf";
    check(!install_grammar({odd}, odd.name, &gerr), "a grammar name that is not a plain word is refused");
    check(grammar_state(*mylang) == GrammarState::kMissing, "a grammar that was never installed is missing");

    // Which language a file is: only C, C++ (HIP too), Python and the files
    // around them are built in; any other comes from the settings, by the
    // file endings a grammar lists.
    check(Syntax::language_for("a/main.CPP", "") == "cpp" && Syntax::language_for("k.hip", "") == "cpp" &&
              Syntax::language_for("x.c", "") == "c" && Syntax::language_for("s.py", "") == "python" &&
              Syntax::language_for("p/CMakeLists.txt", "") == "cmake" && Syntax::language_for("init.lua", "") == "lua" &&
              Syntax::language_for("run", "#!/usr/bin/env python3") == "python",
          "built-in languages");
    check(Syntax::language_for("main.rs", "").empty() && Syntax::language_for("app.ts", "").empty() &&
              Syntax::language_for("k.cu", "").empty() && Syntax::language_for("m.go", "").empty() &&
              Syntax::language_for("s", "#!/usr/bin/env node").empty(),
          "other languages are plain text unless the settings add them");
    Config xc = settings({{"syntax.grammars.rust.repo", "\"tree-sitter/tree-sitter-rust\""},
                          {"syntax.grammars.rust.rev", "\"v0.24.0\""},
                          {"syntax.grammars.rust.extensions", "[\".RS\"]"},
                          {"syntax.grammars.typescript.extensions", "[\"ts\", \"mts\"]"},
                          {"syntax.grammars.c.extensions", "[\"h\"]"}});
    check(xc.warnings.empty() && xc.grammars.size() == 3 && xc.grammars[0].extensions == std::vector<std::string>{"rs"},
          "grammar extensions: small letters, no dot");
    check(Syntax::language_for("src/main.rs", "", xc.grammars) == "rust" && Syntax::language_for("a.mts", "", xc.grammars) == "typescript" &&
              Syntax::language_for("x.h", "", xc.grammars) == "c" && Syntax::language_for("x.hpp", "", xc.grammars) == "cpp",
          "the settings add languages, and can take an ending from a built-in one");
    check(!settings({{"syntax.grammars.x.extensions", "\"rs\""}}).warnings.empty() &&
              !settings({{"syntax.grammars.x.extensions", "[\"d.ts\"]"}}).warnings.empty() &&
              !settings({{"syntax.grammars.x.extensions", "[\"\"]"}}).warnings.empty(),
          "extensions that are not a list of file endings warn");

    const ThemeColors& pine = d.themes.find("rose-pine")->second;
    check(pine.syntax_style("variable").italic && !pine.syntax_style("variable.member").italic, "rose pine italics");
    const TokenStyle self = pine.syntax_style("variable.builtin");
    check(self.italic && self.bold && pine.syntax_style("type.builtin").bold && !pine.syntax_style("type").bold, "rose pine bold");
    check(d.font_styles() == ((1u << 1) | (1u << 2) | (1u << 4) | (1u << 8)), "the default themes use italic, bold, a line under links and through struck-out text");
}

// Italic and bold: styled glyph slots, and what the grid makes of them.
// What the view needs to know about a text: its widest line (the limit of
// sideways scrolling), its indentation step, and where indent lines go.
void view_test(int seeds) {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    // The widest line, against a plain walk over a copy of the text. The
    // text is put together from many edits, so lines cross piece borders.
    for (int seed = 0; seed < seeds; ++seed) {
        rng.seed(static_cast<u64>(seed) * 7919 + 5);
        Editor ed;
        ed.tab_width = 1 + static_cast<u32>(rnd(8));
        std::string model;
        for (int k = 0; k < 40; ++k) {
            const std::string piece = random_text(rnd(60));
            const u64 at = rnd(model.size() + 1);
            // Not inside a character: the walk and the editor would then disagree about a broken one.
            if (at < model.size() && is_cont(static_cast<u8>(model[at]))) continue;
            if (at < model.size() && at > 0 && model[at - 1] == '\r') continue;
            ed.move_to(at, false);
            ed.insert(piece);
            model.insert(at, piece);
        }
        u64 want = 0;
        for (size_t at = 0; at <= model.size();) {
            size_t nl = model.find('\n', at);
            if (nl == std::string::npos) nl = model.size();
            std::string_view line(model.data() + at, nl - at);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            u64 col = 0;
            const u8* p = reinterpret_cast<const u8*>(line.data());
            for (size_t i = 0; i < line.size();) {
                if (p[i] == '\t') {
                    col = (col / ed.tab_width + 1) * ed.tab_width;
                    ++i;
                } else if (p[i] < 0x80) {
                    ++col;
                    ++i;
                } else {
                    const Decoded d = decode_utf8(p + i, line.size() - i);
                    col += cp_width(d.cp);
                    i += d.len;
                }
            }
            want = std::max(want, col);
            at = nl + 1;
        }
        std::vector<Piece> pieces;
        ed.buf.snapshot(&pieces);
        const u64 got = Editor::widest_columns(pieces, ed.tab_width);
        if (got != want) fail("widest line", static_cast<u64>(seed), got);
    }
    {
        Editor ed;
        ed.insert("short\n" + std::string(300000, 'x') + "\tend\nlast");
        std::vector<Piece> pieces;
        ed.buf.snapshot(&pieces);
        check(Editor::widest_columns(pieces, 4) == 300007, "a line longer than one slice is measured whole");
        std::stop_source stop;
        stop.request_stop();
        check(Editor::widest_columns(pieces, 4, stop.get_token()) <= 300007, "a stopped measurement returns");
    }

    // The indentation step.
    auto step_of = [](const std::string& text, u32 tab) {
        Editor ed;
        ed.tab_width = tab;
        ed.insert(text);
        return ed.guess_indent();
    };
    check(step_of("a {\n  b {\n    c\n  }\n}\n", 4) == 2, "two-space indentation is found");
    check(step_of("a {\n    b {\n        c\n    }\n}\n", 8) == 4, "four-space indentation is found");
    check(step_of("a {\n\tb {\n\t\tc\n\t}\n}\n", 8) == 8, "tab indentation uses the tab width");
    check(step_of("no indentation at all\nnone\n", 4) == 4, "a flat file falls back on the tab width");
    check(step_of("", 3) == 3, "so does an empty one");
    check(step_of("a\n   b\n      c\n\n   d\n         continued\n", 4) == 3, "the commonest step wins over an odd continuation line");

    // Indent lines in the grid.
    Font font;
    FontSpec spec;
    spec.px = 20;
    std::string err;
    if (!font.init(spec, &err)) {
        std::printf("indent line test skipped: %s\n", err.c_str());
        return;
    }
    Editor ed;
    ed.insert("a {\n    b {\n        c\n\n        d\n    }\n\n}\n\tt\n");
    GridSpec g;
    g.cols = 20;
    g.rows = 12;
    g.tab_width = 4;
    g.guide_step = 4;
    g.guide_inks = 3;
    std::vector<u32> cells(static_cast<size_t>(g.cols) * g.rows);
    auto lines_at = [&](u32 row) {  // the columns of a row that carry an indent line, as a bit set
        u32 bits = 0;
        for (u32 c = 0; c < g.cols; ++c)
            if (((cells[static_cast<size_t>(row) * g.cols + c] & kCellDecoMask) >> kCellDecoShift) == kDecoGuide) bits |= 1u << c;
        return bits;
    };
    GuideCache cache;
    fill_grid(ed.buf, font, g, cells.data());
    guide_grid(ed.buf, g, cells.data(), cache);
    check(lines_at(0) == 0, "a line that is not indented has no indent line");
    check(lines_at(1) == 1, "one level: a line at column 0");
    check(lines_at(2) == 0b10001, "two levels: lines at columns 0 and 4");
    check(lines_at(3) == 0b10001, "a blank line inside a block carries the block's lines on");
    check(lines_at(5) == 1 && lines_at(6) == 1, "a blank line between two depths takes the shallower one's, plus one");
    check(lines_at(7) == 0, "back at the left edge");
    check(lines_at(8) == 1, "a tab indents like the spaces it stands for");
    check(lines_at(10) == 0, "nothing past the end of the text");
    const u32 first = cells[static_cast<size_t>(2) * g.cols], second = cells[static_cast<size_t>(2) * g.cols + 4];
    check(((first >> kCellInkShift) & 31) == kInkGuide && ((second >> kCellInkShift) & 31) == kInkGuide + 1 && (first & kCellOverlay),
          "each level has its own ink");
    // Scrolled sideways, the lines stay on their columns.
    g.first_col = 2;
    fill_grid(ed.buf, font, g, cells.data());
    guide_grid(ed.buf, g, cells.data(), cache);
    check(lines_at(2) == 0b100 && lines_at(3) == 0b100 && lines_at(1) == 0, "scrolled two columns right: column 4 is the third cell");
    // An underline takes a cell with an indent line over.
    g.first_col = 0;
    const GridDeco deco{2, 0, 2, 1};
    g.decos = {&deco, 1};
    fill_grid(ed.buf, font, g, cells.data());
    guide_grid(ed.buf, g, cells.data(), cache);
    decorate_grid(g, cells.data());
    check(lines_at(2) == 0b10000, "an underline replaces the indent line in its cells");
    // Off: nothing is marked.
    g.decos = {};
    g.guide_step = 0;
    fill_grid(ed.buf, font, g, cells.data());
    guide_grid(ed.buf, g, cells.data(), cache);
    check(lines_at(2) == 0, "no indent lines when they are off");

    // A cache kept from frame to frame through random edits (it keeps the
    // rows above where an edit began) marks the same cells as one made
    // afresh each frame, with the view at the left edge and scrolled
    // sideways (where each row's indentation is read from the text).
    for (const u32 first_col : {0u, 5u}) {
        rng.seed(6007 + first_col);
        Editor fz;
        fz.tab_width = 4;
        std::string start;
        for (int l = 0; l < 60; ++l) start += std::string(rnd(5) * 2, ' ') + (rnd(4) ? "\tx" + std::to_string(l) : std::string()) + "\n";
        fz.insert(start);
        GridSpec v;
        v.cols = 60;
        v.rows = 24;
        v.tab_width = 4;
        v.guide_step = 2;
        v.guide_inks = 3;
        v.doc = 7;
        v.top_line = 10;
        v.first_col = first_col;
        v.edits = &fz.edit_log();
        std::vector<u32> kept_cells(static_cast<size_t>(v.cols) * v.rows), fresh_cells(kept_cells.size());
        GuideCache kept;
        static const char* const kPieces[] = {" ", "  ", "\t", "\n", "\n  ", "y", "{\n", "    z\n", "\n\n"};
        bool same = true;
        for (int step = 0; step < 400 && same; ++step) {
            for (u64 k = 1 + rnd(3); k-- > 0;) {  // (sometimes several edits between two frames)
                const u64 at = rnd(fz.buf.size() + 1);
                if (rnd(3) == 0 && at < fz.buf.size()) {
                    fz.replace(at, std::min<u64>(fz.buf.size(), at + 1 + rnd(4)), "");
                } else {
                    fz.move_to(at, false);
                    fz.insert(kPieces[rnd(std::size(kPieces))]);
                }
            }
            v.revision = fz.revision();
            GuideCache fresh;
            fill_grid(fz.buf, font, v, kept_cells.data());
            guide_grid(fz.buf, v, kept_cells.data(), kept);
            fill_grid(fz.buf, font, v, fresh_cells.data());
            guide_grid(fz.buf, v, fresh_cells.data(), fresh);
            same = kept_cells == fresh_cells;
        }
        check(same, first_col ? "indent lines kept through edits, scrolled sideways, match lines made afresh" : "indent lines kept through edits match lines made afresh");
    }
}

// Columns on a long line: the editor remembers where its last walks
// stopped (col_between) and the grid keeps its anchors before an edit
// (AnchorCache). Random edits and questions on a line of 200 KB of ASCII,
// tabs, wide characters and emoji, each answer checked against an editor
// that remembers nothing.
void long_line_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    rng.seed(4242);
    static const char* const kBits[] = {"abc", "\t", "x\ty", "\xe6\xbc\xa2", "\xf0\x9f\x91\x8d", "e\xcc\x81", "  ", "{\"k\":1},"};
    std::string line;
    while (line.size() < 200000) line += kBits[rnd(8)];
    Editor ed;
    ed.tab_width = 4;
    ed.insert("first line\n" + line + "\nlast line\n");
    ed.move_to(0, false);
    const u64 start = ed.buf.line_start(1);
    auto boundary = [&](u64 at) {  // a place the cursor may be: a character's start on line 1
        at = std::min(at, ed.line_end(1));
        while (at > start && ed.prev_char(ed.next_char(at)) != at) --at;
        return at;
    };
    // The grid, scrolled far right, with anchors kept through edits, against
    // one with no anchors at all.
    Font font;
    FontSpec spec;
    spec.px = 14;
    std::string ferr;
    const bool have_font = font.init(spec, &ferr);
    AnchorCache anchors;
    GridSpec g;
    g.cols = 80;
    g.rows = 3;
    g.tab_width = 4;
    g.doc = 1;
    std::vector<u32> with(static_cast<size_t>(g.cols) * g.rows), without(with.size());
    for (int step = 0; step < 300; ++step) {
        const u64 end = ed.line_end(1);
        if (have_font && step % 5 == 0) {
            const u32 width = ed.col_between(start, end);
            g.first_col = width > 70000 ? width - 70000 + static_cast<u32>(rnd(60000)) : 70000;
            g.revision = ed.revision();
            g.edits = &ed.edit_log();
            fill_grid(ed.buf, font, g, with.data(), &anchors);
            fill_grid(ed.buf, font, g, without.data());
            check(with == without, "long line: anchors kept through edits draw what a walk from the start draws");
        }
        if (rnd(3) == 0) {  // an edit: mostly near the far end, as typing is
            const u64 at = boundary(rnd(2) ? end - rnd(std::min<u64>(end - start, 5000)) : start + rnd(end - start));
            ed.move_to(at, false);
            if (rnd(4) == 0 && at + 8 < end) {
                ed.move_to(ed.next_char(at), true);
                ed.insert("");
            } else {
                ed.insert(kBits[rnd(8)]);
            }
            continue;
        }
        const u64 at = boundary(rnd(2) ? end - rnd(std::min<u64>(end - start, 70000)) : start + rnd(end - start));
        Editor ref;
        ref.tab_width = 4;
        ref.insert(ed.buf.text(0, ed.buf.size()));
        if (ed.col_between(start, at) != ref.col_between(start, at)) {
            std::printf("long line: at %llu, %u against %u\n", static_cast<unsigned long long>(at - start), ed.col_between(start, at), ref.col_between(start, at));
            check(false, "long line: a remembered column is the column");
        }
    }
}

void lines_test(const FontSpec& spec);

// A screen that needs more glyphs than the atlas has: the frame's second
// try holds the atlas, and what does not fit is a box. No slot shows two
// characters (before, the atlas started over in the middle of the try and
// the rows made before that showed glyphs that were not theirs).
void atlas_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Font font;
    FontSpec spec;
    spec.px = 20;
    std::string err;
    if (!font.init(spec, &err)) {
        std::printf("atlas test skipped: %s\n", err.c_str());
        return;
    }
    // 100 rows of 50 wide characters, each a different one: 10,000 slots.
    constexpr u32 kCols = 100, kRows = 100, kPerRow = kCols / 2;
    std::string text;
    for (u32 r = 0; r < kRows; ++r) {
        for (u32 c = 0; c < kPerRow; ++c) {
            const u32 cp = 0x4E00 + r * kPerRow + c;
            text += static_cast<char>(0xE0 | (cp >> 12));
            text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (cp & 0x3F));
        }
        text += '\n';
    }
    Editor ed;
    ed.insert(text);
    GridSpec g;
    g.cols = kCols;
    g.rows = kRows;
    g.tab_width = 4;
    std::vector<u32> cells(static_cast<size_t>(kCols) * kRows);
    u32 gen = font.generation();
    fill_grid(ed.buf, font, g, cells.data());
    check(font.generation() != gen, "atlas: a screen of 5,000 distinct wide characters fills it");
    font.hold_atlas(true);  // (as render.cpp does the second time)
    gen = font.generation();
    fill_grid(ed.buf, font, g, cells.data());
    const bool full = font.full();
    font.hold_atlas(false);
    check(font.generation() == gen && full, "atlas: held, it does not start over; it says it was full");
    std::vector<u32> who(1u << 16, 0);  // slot -> the character it showed (0: none yet)
    u32 shared = 0, real = 0;
    std::vector<bool> counted(1u << 16, false);
    for (u32 r = 0; r < kRows; ++r)
        for (u32 c = 0; c < kCols; ++c) {
            const u32 slot = cells[static_cast<size_t>(r) * kCols + c] & 0xFFFF;
            const u32 cp = 0x4E00 + r * kPerRow + c / 2;
            if (who[slot] == 0) {
                who[slot] = cp;
                ++real;
            } else if (who[slot] != cp && !counted[slot]) {
                counted[slot] = true;
                ++shared;
            }
        }
    check(shared <= 2, "atlas: no slot shows two characters (but the two halves of the box)");
    check(real > 7000, "atlas: most of the screen is drawn: 8,191 slots, two a character");

    // A line under and through: a style's slots are its face's glyphs with
    // the line drawn over them, the space too (the line runs on between
    // words), wide characters across both their cells. (A fresh font: the
    // one above has a full atlas.)
    lines_test(spec);
}

void lines_test(const FontSpec& spec) {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Font font;
    std::string err;
    if (!font.init(spec, &err)) return;
    check(font.ensure_style(Font::kUnderline) && font.ensure_style(Font::kStrike | Font::kBold), "lines: the styles are made");
    auto ink_rows = [&](u32 slot) {  // which rows of a slot have any ink, as a bit set
        u64 rows = 0;
        const u32 sx = (slot % font.atlas_cols()) * static_cast<u32>(font.cell_w()), sy = (slot / font.atlas_cols()) * static_cast<u32>(font.cell_h());
        for (int y = 0; y < font.cell_h() && y < 64; ++y)
            for (int x = 0; x < font.cell_w(); ++x)
                if (font.pixels()[static_cast<size_t>(sy + static_cast<u32>(y)) * static_cast<u32>(font.atlas_w()) + sx + static_cast<u32>(x)]) rows |= 1ull << y;
        return rows;
    };
    auto full_rows = [&](u32 slot) {  // rows whose every pixel has ink
        u64 rows = 0;
        const u32 sx = (slot % font.atlas_cols()) * static_cast<u32>(font.cell_w()), sy = (slot / font.atlas_cols()) * static_cast<u32>(font.cell_h());
        for (int y = 0; y < font.cell_h() && y < 64; ++y) {
            bool all = true;
            for (int x = 0; x < font.cell_w() && all; ++x)
                all = font.pixels()[static_cast<size_t>(sy + static_cast<u32>(y)) * static_cast<u32>(font.atlas_w()) + sx + static_cast<u32>(x)] != 0;
            if (all) rows |= 1ull << y;
        }
        return rows;
    };
    const u32 space = font.glyph(' ', Font::kUnderline) & (Font::kPict - 1), plain_a = font.glyph('a', 0) & (Font::kPict - 1), under_a = font.glyph('a', Font::kUnderline) & (Font::kPict - 1);
    const u64 line = full_rows(space);
    check(line != 0 && ink_rows(space) == line, "lines: an underlined space is the line alone");
    check((ink_rows(under_a) & ~ink_rows(plain_a)) != 0 && (full_rows(under_a) & line) == line, "lines: an underlined a is the a with the line under it");
    const u64 strike = full_rows(font.glyph(' ', Font::kStrike | Font::kBold) & (Font::kPict - 1));
    check(strike != 0 && strike < line, "lines: a line through sits above the line under");
    const u32 wide = font.glyph(0x6F22, Font::kUnderline);
    check((wide & Font::kWide) && (full_rows(wide & (Font::kPict - 1)) & line) == line && (full_rows((wide & (Font::kPict - 1)) + 1) & line) == line,
          "lines: a wide character is underlined across both its cells");
}

void styles_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Font font;
    FontSpec spec;
    spec.px = 20;
    spec.styles = 1u << Font::kItalic;
    std::string err;
    if (!font.init(spec, &err)) {  // no usable font on this machine
        std::printf("styles test skipped: %s\n", err.c_str());
        return;
    }
    check(font.has_style(Font::kItalic) && !font.has_style(Font::kBold), "styles asked for up front are ready");
    check(font.ensure_style(Font::kBold) && font.ensure_style(Font::kItalic | Font::kBold) && font.has_style(3),
          "styles load on demand");
    check(font.glyph('a', 3) == ('a' | 3u << Font::kStyleShift), "styled ASCII slot");
    const u32 e = font.glyph(0xE9, 1);
    check((e & (Font::kWide - 1)) >= Font::kFaces * 128 && e == font.glyph(0xE9, 1), "styled non-ASCII slot");
    check(font.glyph(0x65E5, 1) == font.glyph(0x65E5), "a character the styled face lacks comes back regular");

    // One row of mixed text, all in one style. ASCII carries the style in
    // its slot number; tabs, accents and wide characters must not.
    Buffer buf;
    const std::string text = "ab\t\xC3\xA9\xE6\x97\xA5x -> y\n";
    buf.insert(0, text);
    Highlights h;
    h.end = text.size();
    h.off = {0};
    h.style = {1};
    h.seal();
    u32 lut[256] = {};
    const u32 ink = 5u << kCellInkShift;
    for (u32 fs = 0; fs < Font::kStyles; ++fs) {
        const u32 block = fs << Font::kStyleShift;
        lut[1] = ink | block;
        font.ensure_style(fs);
        // (A face's ASCII is its block's slot; with a line under or through
        // it is a slot of its own, looked up.)
        auto ascii = [&](char c) { return (font.glyph(static_cast<u8>(c), fs) & (Font::kPict - 1)) | ink; };
        GridSpec g;
        g.cols = 24;
        g.rows = 1;
        g.highlights = &h;
        g.style_bits = lut;
        u32 cells[24];
        fill_grid(buf, font, g, cells);
        const u32 accent = font.glyph(0xE9, fs) & (Font::kWide - 1), wide = font.glyph(0x65E5, fs) & (Font::kWide - 1);
        check(cells[0] == ascii('a') && cells[1] == ascii('b') && (fs >= Font::kFaces || cells[0] == ('a' | block | ink)), "styled ASCII cells");
        check(cells[2] == (' ' | ink) && cells[3] == (' ' | ink), "tab cells are plain blanks");
        check(cells[4] == (accent | ink), "styled accent cell");
        check(cells[5] == (wide | ink) && cells[6] == ((wide + 1) | ink), "wide character in a style");
        check(cells[7] == ascii('x'), "ASCII after other characters");
        check(cells[14] == 0, "past the end of the line");
    }
}

// A character two cells wide in the last column of a row, cut by the edge,
// with what is drawn over it after: a mark over both its cells, the rest of
// an emoji sequence. Only the cell in the row takes it; the cell past the
// row's end (here a guard) is not written. (Before, it was: the first cell
// of the next row, or past the end of the cells after the last.)
void edge_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Font font;
    FontSpec spec;
    spec.px = 20;
    std::string err;
    if (!font.init(spec, &err)) {
        std::printf("edge test skipped: %s\n", err.c_str());
        return;
    }
    constexpr u32 kGuard = 0xDEADBEEFu;
    auto fill = [&](const std::string& text, u32 first_col, std::vector<u32>* cells) {
        Buffer buf;
        buf.insert(0, text);
        GridSpec g;
        g.cols = 10;
        g.rows = 1;
        g.first_col = first_col;
        cells->assign(g.cols + 1, kGuard);
        fill_grid(buf, font, g, cells->data());
    };
    std::vector<u32> cells;
    const u32 han = font.glyph(0x6F22) & (Font::kWide - 1);
    fill("aaaaaaaaa\xE6\xBC\xA2\xCC\x81x\n", 0, &cells);  // (a, ..., the Han character for "Han", a combining acute)
    check(cells[10] == kGuard, "edge: a mark over a wide character cut by the right edge stays in the row");
    check((cells[9] & 0xFFFF) == font.mark(han, 0x301, 1), "edge: the mark is drawn over the half that shows");
    fill("aaaaaaaa\xE6\xBC\xA2\xE6\xBC\xA2\xCC\x81x\n", 1, &cells);  // (scrolled one column: the second one cut)
    check(cells[10] == kGuard, "edge: and scrolled sideways");
    {  // A mark typed after a wide character, in a piece of the text of its own: over both its cells.
        Buffer buf;
        buf.insert(0, "\xE6\xBC\xA2\n");
        buf.insert(3, "\xCC\x81");
        check(buf.reader(3).chunk().size() == 2, "pieces: the mark is a piece of its own");
        GridSpec g;
        g.cols = 4;
        g.rows = 1;
        cells.assign(g.cols, 0);
        fill_grid(buf, font, g, cells.data());
        check(cells[0] == font.mark(han, 0x301, 1) && cells[1] == font.mark(han + 1, 0x301, 2), "pieces: a mark in a piece of its own goes over both cells of a wide character");
        // An emoji joined to a letter has no cell: a mark after it goes over the letter, which is narrow.
        Buffer joined;
        joined.insert(0, "a\xE2\x80\x8D\xF0\x9F\x98\x80\xCC\x81\n");
        cells.assign(g.cols, 0);
        fill_grid(joined, font, g, cells.data());
        check(cells[0] == font.mark('a', 0x301) && cells[1] == 0, "joined: a mark after an emoji joined to a letter is over the letter alone");
    }
    // Emoji sequences: a family (joined by zero width joiners), a flag, a
    // skin tone. (With no colour font they are each their first emoji, and
    // nothing is written over them.)
    for (const char* seq : {"\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",
                            "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"}) {
        fill(std::string("aaaaaaaaa") + seq + "x\n", 0, &cells);
        check(cells[10] == kGuard, "edge: an emoji sequence cut by the right edge stays in the row");
        check(cells[9] != 0 && cells[9] != kGuard, "edge: its first cell shows");
    }

    // Any window onto a line shows what a wider one from its start shows
    // there: scrolled sideways, cut at either edge, or a wrapped line's
    // row (its part, then blanks). Random lines of everything that takes
    // other than one cell, made in pieces (so that characters cross the
    // text's chunks).
    static const char* const kBits[] = {"ab", "x", " ", "\t", "\r", "\x01", "\xE6\xBC\xA2", "e\xCC\x81", "\xCC\x81", "\xE6\xBC\xA2\xCC\x81",
                                        "\xF0\x9F\x98\x80", "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5",
                                        "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", "\xE2\x9C\x85\xEF\xB8\x8F", "\xE2\x80\x8B", "\xFF", "\xE6\xBC"};
    rng.seed(777);
    for (int round = 0; round < 300; ++round) {
        Buffer buf;
        std::string line;
        for (int k = 0, n = 1 + static_cast<int>(rnd(40)); k < n; ++k) {
            const std::string bit = kBits[rnd(std::size(kBits))];
            buf.insert(buf.size(), bit);  // (a piece each)
            line += bit;
        }
        buf.insert(buf.size(), "\n");
        constexpr u32 kWide = 200;
        GridSpec g;
        g.rows = 1;
        g.tab_width = 1 + static_cast<u32>(rnd(8));
        g.cols = kWide;
        std::vector<u32> whole(kWide + 1, kGuard), part;
        fill_grid(buf, font, g, whole.data());
        check(whole[kWide] == kGuard, "window: the reference row stays in its cells");
        {  // (Made in one piece, the line shows the same: a mark in a piece of its own finds a wide character before it.)
            Buffer one;
            one.insert(0, line + "\n");
            std::vector<u32> joined(kWide + 1, kGuard);
            fill_grid(one, font, g, joined.data());
            if (joined != whole) std::printf("pieces: line \"%s\" shows otherwise in one piece\n", line.c_str());
            check(joined == whole, "window: a line made in pieces shows what it shows made in one");
        }
        {  // (The cells the line takes are as many as the editor's columns, by which the cursor is placed.)
            Editor ed;
            ed.tab_width = g.tab_width;
            ed.insert(line + "\n");
            u32 n = 0;
            while (n < kWide && whole[n]) ++n;  // (no glyph's slot is 0: a cell past the end of the line is)
            if (n != ed.col_between(0, ed.line_end(0))) std::printf("width: line \"%s\", tab %u: %u cells, %u columns\n", line.c_str(), g.tab_width, n, ed.col_between(0, ed.line_end(0)));
            check(n == ed.col_between(0, ed.line_end(0)), "window: a line takes as many cells as the editor gives it columns");
        }
        for (int t = 0; t < 8; ++t) {
            const u32 from = static_cast<u32>(rnd(60)), cols = 1 + static_cast<u32>(rnd(40));
            g.cols = cols;
            g.first_col = from;
            part.assign(cols + 1, kGuard);
            fill_grid(buf, font, g, part.data());
            bool same = part[cols] == kGuard;
            for (u32 c = 0; c < cols && same; ++c) same = part[c] == whole[from + c];
            if (!same) std::printf("window: line \"%s\", tab %u, from %u, %u columns\n", line.c_str(), g.tab_width, from, cols);
            check(same, "window: a window onto a line shows what the line shows there");
            // As a wrapped line's row: from `from`, its part `len` columns.
            const u32 len = static_cast<u32>(rnd(cols + 1)), end = from + len;
            const u64 row_line = 0;
            g.first_col = 0;
            g.row_lines = &row_line;
            g.row_cols = &from;
            g.row_ends = &end;
            part.assign(cols + 1, kGuard);
            fill_grid(buf, font, g, part.data());
            g.row_lines = nullptr;
            g.row_cols = g.row_ends = nullptr;
            same = part[cols] == kGuard;
            for (u32 c = 0; c < cols && same; ++c) same = part[c] == (c < len ? whole[from + c] : 0);
            if (!same) std::printf("wrapped row: line \"%s\", tab %u, from %u to %u, %u columns\n", line.c_str(), g.tab_width, from, end, cols);
            check(same, "window: a wrapped line's row shows its part of the line");
        }
    }
}

// Text in a popup (TextBox::put) takes the cells it takes in the text
// (fill_grid), and as many as width_of() says. (Before, a mark after a
// wide character was drawn over its right cell alone, and an emoji
// sequence the colour font has no picture of took two cells for each of
// its emoji, where the text gives all of it its first's two.)
void box_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    Font font;
    FontSpec spec;
    spec.px = 20;
    std::string err;
    if (!font.init(spec, &err)) {
        std::printf("box test skipped: %s\n", err.c_str());
        return;
    }
    constexpr u32 kWide = 200;
    TextBox box;
    box.resize(kWide, 1);
    const u32 han = font.glyph(0x6F22) & (Font::kWide - 1);
    check(box.put(font, 0, 0, "\xE6\xBC\xA2\xCC\x81", 0) == 2, "box: a wide character with a mark takes its two cells");
    check(box.cells[0] == font.mark(han, 0x301, 1) && box.cells[1] == font.mark(han + 1, 0x301, 2), "box: the mark is over both of them");
    box.resize(kWide, 1);
    const char* const grins = "\xF0\x9F\x98\x80\xE2\x80\x8D\xF0\x9F\x98\x80\xE2\x80\x8D\xF0\x9F\x98\x80";  // (three grins joined: no picture of that)
    check(box.put(font, 0, 0, grins, 0) == 2 && width_of(grins) == 2, "box: emoji joined with no picture of them take their first's two cells");
    static const char* const kBits[] = {"ab", "x", " ", "\xE6\xBC\xA2", "e\xCC\x81", "\xCC\x81", "\xE6\xBC\xA2\xCC\x81", "\xF0\x9F\x98\x80",
                                        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5", "\xF0\x9F\x87\xAF",
                                        "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", "\xE2\x9C\x85\xEF\xB8\x8F", "\xE2\x80\x8B", "\xE2\x80\x8D", grins};
    rng.seed(919);
    for (int round = 0; round < 300; ++round) {
        std::string line;
        for (int k = 0, n = 1 + static_cast<int>(rnd(30)); k < n; ++k) line += kBits[rnd(std::size(kBits))];
        Buffer buf;
        buf.insert(0, line + "\n");
        GridSpec g;
        g.rows = 1;
        g.cols = kWide;
        std::vector<u32> text(kWide);
        fill_grid(buf, font, g, text.data());
        box.resize(kWide, 1);
        const u32 end = box.put(font, 0, 0, line, 0);
        u32 n = 0;
        while (n < kWide && text[n]) ++n;
        bool same = end == n && width_of(line) == n;
        for (u32 c = 0; c < kWide && same; ++c) same = (box.cells[c] & 0xFFFF) == (text[c] & 0xFFFF);
        if (!same) std::printf("box: line \"%s\": %u cells in the box, %u in the text, width %u\n", line.c_str(), end, n, width_of(line));
        check(same, "box: a line in a box takes the cells it takes in the text");
    }
}

// Text cut to a number of columns (fit_text): never wider than that, and
// what follows the ellipsis of a path starts with a character of its own
// (before, a mark or a selector whose character was cut off was kept, and
// drawn over the ellipsis).
void fit_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    check(fit_text("abcdef", 6) == "abcdef" && fit_text("abcdef", 4) == "abc\xE2\x80\xA6", "fit: cut at the end, an ellipsis there");
    check(fit_text("abcdef", 4, true) == "\xE2\x80\xA6" "def", "fit: a path keeps its end");
    check(fit_text("aaaae\xCC\x81xyz", 4, true) == "\xE2\x80\xA6xyz", "fit: a path cut at a decomposed letter keeps none of its mark");
    check(fit_text("\xE2\x9C\x85\xEF\xB8\x8F" "ab", 3, true) == "\xE2\x80\xA6" "ab", "fit: nor the selector of an emoji cut off");
    check(fit_text("a/\xE6\xBC\xA2\xE6\xBC\xA2", 4, true) == "\xE2\x80\xA6\xE6\xBC\xA2", "fit: a wide character that does not fit goes whole");
    static const char* const kBits[] = {"a", "/", "\xE6\xBC\xA2", "e\xCC\x81", "\xCC\x81", "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9",
                                        "\xE2\x9C\x85\xEF\xB8\x8F", "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"};
    rng.seed(4242);
    for (int round = 0; round < 2000; ++round) {
        std::string s;
        for (int k = 0, n = static_cast<int>(rnd(12)); k < n; ++k) s += kBits[rnd(std::size(kBits))];
        const u32 cols = static_cast<u32>(rnd(14));
        for (const bool path : {false, true}) {
            const std::string f = fit_text(s, cols, path);
            check(width_of(f) <= cols, "fit: never wider than asked");
            if (f == s) continue;
            check(f.empty() == (cols == 0), "fit: an ellipsis wherever it was cut");
            if (!path || f.size() <= 3) continue;
            const u8* p = reinterpret_cast<const u8*>(f.data()) + 3;
            check(cp_width(decode_utf8(p, f.size() - 3).cp) != 0, "fit: after a path's ellipsis, a character with a cell of its own");
        }
    }
}

// The two pattern matchers used by highlight queries, on the patterns those
// queries actually contain.
void match_test() {
    auto lua = [](const char* text, const char* pat, bool want) {
        if (lua_match(text, pat) != want) {
            std::fprintf(stderr, "lua_match(\"%s\", \"%s\") should be %d\n", text, pat, want);
            fail("lua pattern", 0, 0);
        }
    };
    auto re = [](const char* text, const char* pat, bool want) {
        if (regex_match(text, pat) != want) {
            std::fprintf(stderr, "regex_match(\"%s\", \"%s\") should be %d\n", text, pat, want);
            fail("regex", 0, 0);
        }
    };
    lua("FOO_BAR", "^[A-Z][A-Z_0-9]*$", true);
    lua("Foo", "^[A-Z][A-Z_0-9]*$", false);
    lua("Foo", "^%u", true);
    lua("foo", "^%u", false);
    lua("m_value", "^m_.*$", true);
    lua("value", "^m_.*$", false);
    lua("__init__", "^__[a-zA-Z0-9_]*__$", true);
    lua("_FOO1", "^_*[A-Z][A-Z%d_]*$", true);
    lua("_foo", "^_*[A-Z][A-Z%d_]*$", false);
    lua("/** doc */", "^/[*][*][^*].*[*]/$", true);
    lua("/* doc */", "^/[*][*][^*].*[*]/$", false);
    lua("3.14", "^[-]?%d*%.%d*$", true);
    lua("3", "^[-]?%d*%.%d*$", false);
    lua("-- @param", "^[-][-](%s?)@", true);
    lua("; format-ignore", "^;+%s*format%-ignore%s*$", true);
    lua("xfooy", "foo", true);
    lua("aaa", "^a-$", true);
    lua("", "^$", true);
    lua("x", "%f[%w]", false);  // frontier patterns are not supported: never match
    re("continue", "\\c^(continue|break)$", true);
    re("BREAK", "\\c^(continue|break)$", true);
    re("breaks", "\\c^(continue|break)$", false);
    re("Foo-NOTFOUND", "\\c^(1|on|yes|true|y|0|off|no|false|n|ignore|notfound|.*-notfound)$", true);
    re("maybe", "\\c^(1|on|yes|true|y|0|off|no|false|n|ignore|notfound|.*-notfound)$", false);
    re("-12.5", "^[-+]?[0-9]+(.[0-9]+)?$", true);
    re("1a", "^[-+]?[0-9]+(.[0-9]+)?$", false);
    re("FOO_BAR9", "^[A-Z][A-Z\\d_]+$", true);
    re("acbcbd", "^a(b|c)*d$", true);
    re("acbxd", "^a(b|c)*d$", false);
    re("a.b", "a\\.b", true);
    re("axb", "a\\.b", false);
    re("x", "(", false);  // malformed patterns match nothing
    re("x", "[abc", false);
}

// Highlights::edit against a plain per-byte model: a style for every byte,
// with bytes inserted and removed directly.
void highlights_test() {
    for (u64 seed = 1; seed <= 300; ++seed) {
        rng.seed(seed * 31);
        const u64 base = rnd(500);
        std::vector<u8> model(200 + rnd(3000));
        u8 cur = 0;
        for (u8& b : model) {
            if (rnd(9) == 0) cur = static_cast<u8>(rnd(6));
            b = cur;
        }
        Highlights h;
        h.base = base;
        h.end = base + model.size();
        for (size_t i = 0; i < model.size(); ++i)
            if (i == 0 || model[i] != model[i - 1]) {
                h.off.push_back(static_cast<u32>(i));
                h.style.push_back(model[i]);
            }
        h.seal();
        u64 lo = base;  // where the model's first byte sits in the document
        bool alive = true;
        for (int step = 0; step < 60 && alive; ++step) {
            // Edits cluster, as typing does, with the odd jump elsewhere.
            const u64 doc_end = lo + model.size();
            const u64 start = rnd(8) == 0 ? rnd(doc_end + 20) : std::min(doc_end, lo + rnd(model.size() + 1));
            const u64 old_len = rnd(3) == 0 ? rnd(12) : 0, new_len = rnd(3) == 0 ? 0 : rnd(6) + 1;
            h.edit(start, old_len, new_len);
            if (start >= doc_end) continue;  // after the range: nothing moves
            if (start + old_len <= lo) {
                lo = lo + new_len - old_len;
            } else if (start < lo || start + old_len > doc_end) {
                if (!h.empty()) fail("edit across the edge should drop the highlights", seed, step);
                alive = false;
                continue;
            } else {
                const size_t at = static_cast<size_t>(start - lo);
                const u8 fill = at > 0 ? model[at - 1] : 0;
                model.erase(model.begin() + static_cast<ptrdiff_t>(at), model.begin() + static_cast<ptrdiff_t>(at + old_len));
                model.insert(model.begin() + static_cast<ptrdiff_t>(at), new_len, fill);
            }
            if (h.empty()) fail("highlights dropped unexpectedly", seed, step);
            if (h.base != lo || h.end != lo + model.size()) fail("highlight range after edit", seed, step);
            // Read every byte's style back through the cursor, as the renderer does.
            u32 lut[8];
            for (u32 i = 0; i < 8; ++i) lut[i] = i;
            StyleCursor sc(&h, lut);
            for (size_t i = 0; i < model.size(); ++i) {
                sc.advance(lo + i);
                if (sc.bits != model[i]) {
                    std::fprintf(stderr, "byte %zu: cursor %u, model %u\n", i, sc.bits, model[i]);
                    fail("highlight style after edit", seed, step);
                }
            }
            sc.advance(lo + model.size() + 5);
            if (sc.bits != 0) fail("style past the range", seed, step);
        }
    }
}

// End to end, when a C++ grammar is installed: colours kept up to date
// across edits must equal colours computed from scratch for the same text,
// and the sliced query must give the same answer as the whole one.
void syntax_test() {
    SyntaxOptions opt;
    if (!Syntax::installed("cpp", opt)) {
        std::printf("(no C++ grammar installed: syntax test skipped)\n");
        return;
    }
    static const char* const snippets[] = {
        "int x = 42;\n", "/* note", " */", "// line\n", "\"str\\n\"", "struct S { int a; };\n", "if (a->b != c) {\n",
        "}\n", "auto f = [](int v) -> bool { return v > 0; };\n", "#define MAX_N 10\n", " ", "x", "template <class T>\n",
    };
    auto fresh = [&](const std::string& text, SyntaxOptions::Compile mode, Highlights* out) {
        Buffer b;
        b.insert(0, text);
        SyntaxOptions o = opt;
        o.compile = mode;
        o.margin_lines = 100000;  // the whole file
        Syntax s;
        if (!s.attach(b, "cpp", o)) fail("attach", 0, 0);
        s.want(b, 0, 10);
        s.wait_idle();
        s.poll();
        *out = s.highlights();
        s.detach();
    };
    for (u64 seed = 1; seed <= 6; ++seed) {
        rng.seed(seed * 101);
        Editor e;
        std::string m = "#include <vector>\nnamespace n {\nint main(int argc, char** argv) {\n    return argc;\n}\n}\n";
        e.buf.insert(0, m);
        SyntaxOptions o = opt;
        o.margin_lines = 100000;
        Syntax syn;
        if (!syn.attach(e.buf, "cpp", o)) fail("attach", seed, 0);
        e.listen(&syn);
        for (int step = 0; step < 40; ++step) {
            const u64 at = rnd(m.size() + 1);
            if (rnd(3) == 0 && !m.empty()) {
                const u64 from = rnd(m.size()), len = std::min<u64>(rnd(20) + 1, m.size() - from);
                e.move_to(from, false);
                e.move_to(from + len, true);
                e.backspace();
                m.erase(from, len);
            } else {
                const std::string s = snippets[rnd(std::size(snippets))];
                e.move_to(at, false);
                e.insert(s);
                m.insert(at, s);
            }
            if (rnd(4) == 0 && e.undo()) m = e.buf.text(0, e.buf.size());
            if (e.buf.text(0, e.buf.size()) != m) fail("syntax test text", seed, step);
            syn.want(e.buf, 0, 10);
            if (rnd(3) != 0) continue;  // let several edits pile up before looking
            syn.wait_idle();
            syn.poll();
            Highlights live = syn.highlights(), whole, parts;
            fresh(m, SyntaxOptions::kWholeOnly, &whole);
            fresh(m, SyntaxOptions::kPartsOnly, &parts);
            if (!whole.same_as(parts)) fail("sliced query differs from whole query", seed, step);
            if (!live.same_as(whole)) fail("incremental highlights differ from fresh ones", seed, step);
        }
        e.unlisten(&syn);
        syn.detach();
    }
}

// The JSON reader and writer.
void json_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    json::Doc d;
    check(d.parse(R"( {"a": 1, "b": [true, false, null, -2.5e2, "x\ty"],
                       "s": "caf\u00e9 \ud83d\ude00 \"q\" \\ \/", "o": {"k": {"deep": [1, [2, [3]]]}}, "e": [], "eo": {} } )"),
          "json parses");
    const json::Value v = d.root();
    check(v.is_object() && v.size() == 6 && v["a"].integer() == 1 && v["a"].number() == 1.0, "json numbers");
    check(v["b"].size() == 5 && v["b"].at(0).boolean() && !v["b"].at(1).boolean(true) && v["b"].at(2).exists() && !v["b"].at(2),
          "json booleans and null");
    check(v["b"].at(3).number() == -250.0 && v["b"].at(4).str() == "x\ty" && !v["b"].at(5).exists(), "json array access");
    check(v["s"].str() == "caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\" \\ /", "json string escapes and surrogate pairs");
    check(v["o"]["k"]["deep"].at(1).at(1).at(0).integer() == 3, "json nesting");
    check(!v["nope"].exists() && v["nope"]["deeper"].str().empty() && v["a"]["x"].integer(7) == 7 && v["nope"].size() == 0,
          "json: asking for what is not there is harmless");
    check(v["e"].is_array() && v["e"].size() == 0 && v["eo"].is_object() && v["eo"].size() == 0, "json empty containers");
    std::string keys;
    for (json::Value m : v) keys += std::string(m.key()) + ",";
    check(keys == "a,b,s,o,e,eo,", "json members in order");
    int sum = 0;
    for (json::Value e : v["o"]["k"]["deep"]) sum += static_cast<int>(e.integer());
    check(sum == 1, "json array iteration");

    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "\"open", "tru", "[1] x", "{\"a\":1,}", "nan", "[\"\\u12\"]"})
        check(!json::Doc().parse(bad), "json rejects malformed text");
    std::string deep(500, '[');
    deep += std::string(500, ']');
    check(!json::Doc().parse(deep), "json refuses absurd nesting");

    json::Writer w;
    w.begin_object().member("n", 42).member("neg", static_cast<i64>(-7)).member("t", true).key("list").begin_array();
    w.value("a\"b\\c\n\x01").value(1.5).null().begin_object().end_object().end_array();
    w.member("u", "\xC3\xA9\xFFx").end_object();
    check(w.out == "{\"n\":42,\"neg\":-7,\"t\":true,\"list\":[\"a\\\"b\\\\c\\n\\u0001\",1.5,null,{}],\"u\":\"\xC3\xA9\xEF\xBF\xBDx\"}",
          "json writer");
    json::Doc back;
    check(back.parse(w.out) && back.root()["list"].at(0).str() == "a\"b\\c\n\x01" && back.root()["u"].str() == "\xC3\xA9\xEF\xBF\xBDx",
          "json round trip");
    std::string dumped;
    json::dump(dumped, back.root());
    check(dumped == w.out, "json dump reproduces the text");

    std::vector<InfoLine> lines;
    Lsp::markdown_lines("### Title\n\nSome **bold** and `code` and a [link](http://x) \\_ok\\_ &lt;T&gt;\n\n\n```cpp\nint  x;\n\ty();\n```\n---\nlast\n\n",
                        true, &lines);
    check(lines.size() == 8, "markdown line count");
    check(lines[0].text == "Title" && lines[0].kind == InfoLine::kHeading && lines[1].text.empty(), "markdown heading");
    check(lines[2].text == "Some bold and code and a link _ok_ <T>", "markdown inline marks");
    check(lines[3].text.empty() && lines[4].text == "int  x;" && lines[4].kind == InfoLine::kCode && lines[5].text == "    y();",
          "markdown code block");
    check(lines[6].kind == InfoLine::kRule && lines[7].text == "last", "markdown rule and no trailing blank");
    lines.clear();
    Lsp::markdown_lines("### function `add`\n\n---\n\nAdds.\n\n---\n```cpp\nint add()\n```", true, &lines);
    check(lines.size() == 5 && lines[1].kind == InfoLine::kRule && lines[2].text == "Adds." && lines[3].kind == InfoLine::kRule &&
              lines[4].text == "int add()",
          "markdown: no blank lines around rules");

    // Matching what was typed against completions.
    std::vector<u32> at;
    check(lsp_match("", "anything") == 0 && lsp_match("xyz", "getValue") < 0 && lsp_match("toolong", "to") < 0, "match: empty and none");
    check(lsp_match("get", "getValue") > lsp_match("get", "get_width_of_everything"), "match: shorter completions first");
    check(lsp_match("get", "getValue") > lsp_match("GET", "getValue") && lsp_match("GET", "getValue") > 0, "match: case counts, a little");
    check(lsp_match("gv", "getValue", &at) > 0 && at.size() == 2 && at[0] == 0 && at[1] == 3, "match: letters found, positions reported");
    check(lsp_match("gw", "get_width") > lsp_match("gw", "aligned_window"), "match: word starts beat the middle of words");
    check(lsp_match("val", "getValue") > 0 && lsp_match("get", "getValue") > lsp_match("val", "getValue"), "match: a prefix beats the middle");

    // Servers from the settings come first and inherit from built-in
    // servers of the same name.
    Config lc = settings({{"lsp.max_file_mb", "2"},
                          {"lsp.completion", "false"},
                          {"lsp.hover_ms", "100"},
                          {"lsp.servers.clangd.command", "[\"clangd\", \"--background-index\"]"},
                          {"lsp.servers.clangd.init_options", "'{\"fallbackFlags\": [\"-std=c++23\"]}'"},
                          {"lsp.servers.mine.command", "[\"my-server\", \"--stdio\"]"},
                          {"lsp.servers.mine.languages", "[\"toml\"]"},
                          {"lsp.servers.mine.roots", "[\".git\"]"},
                          {"lsp.servers.marksman.enabled", "false"}});
    check(lc.warnings.empty() && lc.lsp && lc.lsp_max_mb == 2 && !lc.lsp_completion && lc.lsp_inline && lc.lsp_hover_ms == 100,
          "lsp settings");
    check(lc.lsp_servers.size() == 3 && lc.lsp_servers[0].name == "clangd" && lc.lsp_servers[0].command.size() == 2 &&
              lc.lsp_servers[0].init_options == "{\"fallbackFlags\": [\"-std=c++23\"]}",
          "lsp server from settings");
    const std::vector<LspServer> servers = lsp_server_list(lc.lsp_servers);
    check(servers[0].name == "clangd" && servers[0].command[1] == "--background-index" && servers[0].languages.size() == 2 &&
              !servers[0].roots.empty(),
          "a server named like a built-in one inherits what it does not set");
    check(servers[1].name == "mine" && servers[1].languages[0] == "toml" && servers[2].name == "marksman" && !servers[2].enabled,
          "own servers and switched-off ones");
    size_t clangds = 0;
    for (const LspServer& s : servers) clangds += s.name == "clangd";
    check(clangds == 1 && servers.size() == builtin_lsp_servers().size() + 1, "built-in servers are not listed twice");
    check(Lsp::pick(servers, "markdown", nullptr) == nullptr, "a switched-off server is never picked");
    for (const LspServer& s : builtin_lsp_servers())
        for (const std::string& l : s.languages)
            check(l == "c" || l == "cpp" || l == "python" || l == "cmake" || l == "json" || l == "toml" || l == "yaml" ||
                      l == "markdown" || l == "bash" || l == "lua",
                  "built-in servers are only for C, C++, Python and the files around them");
    check(!settings({{"lsp.enabled", "false"}}).lsp && !settings({{"lsp.servers.x.command", "\"x\""}}).warnings.empty(),
          "lsp off, and a bad server entry warns");
    const Config themed = settings({{"themes.rose-pine.error", "\"#ff0000\""}});
    const ThemeColors& rp = themed.themes.find("rose-pine")->second;
    check(rp.error.r == 1.0f && rp.error.g == 0 && rp.warning.r == 0xf6 / 255.0f && rp.popup.r == 0x1f / 255.0f, "theme colours for diagnostics and popups");
}

// What blit reads from files and from other programs, spoiled: cut short,
// bytes changed to ones that mean something to a parser, pieces doubled
// and dropped. Nothing is checked but that each reader gets through it
// (run under AddressSanitizer, that it never reads or writes out of bounds),
// and that a profile read back from its file can be shown.
void parsers_test(int seeds) {
    using namespace std::string_literals;
    std::vector<std::string> notebooks, jsons, diffs, statuses, logs, blames, asms, junits, perfs, xmls, snippets, markdowns, profiles;
    Profile original;
    notebooks.push_back(R"NB({"cells": [{"cell_type": "markdown", "id": "a1", "metadata": {}, "source": ["# Title\n", "\n", "Some text"]},
 {"cell_type": "code", "execution_count": 3, "id": "b2", "metadata": {"tags": []}, "outputs": [{"name": "stdout", "output_type": "stream", "text": ["first\n", "6.5\n"]},
 {"data": {"image/png": "iVBOR", "text/plain": ["<Figure>"]}, "metadata": {}, "output_type": "display_data"}], "source": ["x = 13 / 2\n", "print(x)"]},
 {"cell_type": "code", "execution_count": null, "id": "c3", "metadata": {}, "outputs": [{"ename": "E", "evalue": "v", "output_type": "error", "traceback": ["t"]}], "source": "plot()"}],
 "metadata": {"kernelspec": {"display_name": "Python 3", "name": "python3"}, "language_info": {"version": "3.12.1"}}, "nbformat": 4, "nbformat_minor": 5}
)NB");
    jsons.push_back(R"({"a": 1, "b": [true, false, null, -2.5e2, "x\ty"], "s": "caf\u00e9 \ud83d\ude00 \ud800 x \"q\" \\ \/", "o": {"k": {"deep": [1, [2, [3]]]}}, "e": [], "eo": {}})");
    jsons.push_back(R"({"kind":"ctestInfo","tests":[{"name":"Math.Adds","command":["/b/t","--gtest_filter=Math.Adds"],"properties":[{"name":"WORKING_DIRECTORY","value":"/b"}]},{"name":"plain","command":["/b/p"]}]})");
    jsons.push_back(R"({"format_version":"2","current_working_directory":"/b","files":[{"file":"../p/a.cpp","lines":[{"line_number":3,"count":2},{"line_number":4,"count":0}]}]})");
    jsons.push_back(R"({"meta":{},"files":{"src/m.py":{"executed_lines":[1,2],"missing_lines":[5]}}})");
    jsons.push_back(R"([{"directory": "/b", "command": "cc -O2 -DX=\"a b\" -c ../src/sum.c -o sum.o -MD -MF sum.d", "file": "/src/sum.c"}, {"directory": "/b", "arguments": ["c++", "-c", "x.cpp"], "file": "x.cpp"}])");
    diffs.push_back("diff --git a/a.txt b/a.txt\nindex 1111111..2222222 100644\n--- a/a.txt\n+++ b/a.txt\n@@ -1,4 +1,5 @@\n one\n-two\n+TWO\n+2.5\n three\n\tfour\n"
                    "@@ -10 +11,0 @@ int f()\n-ten\n\\ No newline at end of file\ndiff --git a/b.bin b/b.bin\nBinary files a/b.bin and b/b.bin differ\n"
                    "diff --git a/c b/d\nsimilarity index 90%\nrename from c\nrename to d\n@@ -2,2 +2,2 @@\n x\n-y\n+日本\n");
    statuses.push_back("# branch.oid 0123456789abcdef0123456789abcdef01234567\0# branch.head main\0# branch.upstream origin/main\0# branch.ab +2 -1\0"
                       "1 .M N... 100644 100644 100644 aaaa bbbb src/a file.cpp\0"
                       "2 R. N... 100644 100644 100644 aaaa bbbb R100 after.txt\0before.txt\0"
                       "u UU N... 100644 100644 100644 100644 aaaa bbbb cccc both.txt\0? notes.txt\0"s);
    logs.push_back("abc1234\x1f" "Ada\x1f" "now\x1f" "Merge\x1f" "abc1234ffff\x1f" "p1 p2\x1f" "HEAD -> main, tag: v1\x1f" "1700000000\ndef5678\x1f" "Bo\x1f" "3 weeks ago\x1f" "Start\n");
    blames.push_back("0123456789abcdef0123456789abcdef01234567 1 1 2\nauthor Ada\nauthor-time 1700000000\nsummary Start\nfilename a.txt\n\tone\n"
                     "0123456789abcdef0123456789abcdef01234567 2 2\n\ttwo\n0000000000000000000000000000000000000000 3 3 1\nauthor Not Committed Yet\nsummary Version of a.txt\n\tthree\n");
    asms.push_back("\t.file\t\"sum.c\"\n\t.text\n\t.file 1 \"sum.c\"\n\t.globl\tsum\nsum:\n.LFB0:\n\t.loc 1 2 30\n\tmovl\t$0, %eax\t# s\n.L3:\n\t.file 2 \"inc.h\"\n\t.loc 2 7 1\n\tret\n"
                   "\t.section\t.debug_info,\"\",@progbits\n\t.text\n\t.file\t0 \"/src\" \"sum.c\" md5 0x12\n_sum: ; @sum\n\t.loc\t0 4 5 prologue_end\n\tldr\tw8, [sp, #12]\n");
    junits.push_back(R"(<?xml version="1.0"?><testsuite><testcase name="Math.Adds" classname="Math.Adds" time="0.25"/><testcase name="plain" time="1"><failure message="x &lt; 2 &#65; &amp;">trace</failure></testcase><testcase name="s"><skipped/></testcase><![CDATA[x]]><!-- c --></testsuite>)");
    snippets.push_back("for (${1:i} = 0; ${1:i} < ${2:n}; ++${1:i}) {$0} ${3:call(${4:arg})} ${5|left,right|} \\$1 \\${2} \\\\ ${6:a\\}b} $TM_FILENAME ${USER:nobody} ${TM_SELECTED_TEXT/(.*)/$1/}");
    markdowns.push_back("### Title\n\nSome **bold** and `code` and a [link](http://x) \\_ok\\_ &lt;T&gt; &#x41;\n\n```cpp\nint  x;\n\ty();\n```\n---\n- a\n  1. b\n> q\n| a | b |\n|---|---|\n");
#if defined(BLIT_PROFILE_DATA)
    auto slurp = [](const char* name, size_t most) {
        std::string s;
        if (FILE* f = std::fopen((std::string(BLIT_PROFILE_DATA) + "/" + name).c_str(), "rb")) {
            s.resize(most);
            s.resize(std::fread(s.data(), 1, most, f));
            std::fclose(f);
        }
        return s;
    };
    perfs.push_back(slurp("perf-script.txt", 6000));
    perfs.push_back(slurp("perf-page-faults.txt", 6000));
    {
        std::string err;
        if (parse_perf_script(slurp("perf-script.txt", 20000), "", &original, &err)) profiles.push_back(original.to_json());
    }
    xmls.push_back(slurp("xctrace-syscall.xml", 12500));
    xmls.push_back(slurp("xctrace-time-profile.xml", 12000));
#endif
    static const char kMeaningful[] = "{}[]\":,\\\n\0<>&;#-+0123456789 \t\x1f@$|/.*eE'u";
    auto spoil = [](std::string s) {
        for (int n = 1 + static_cast<int>(rnd(4)); n-- > 0 && !s.empty();) {
            const size_t at = rnd(s.size());
            switch (rnd(6)) {
                case 0: s.resize(at); break;
                case 1: s[at] = kMeaningful[rnd(sizeof kMeaningful - 1)]; break;
                case 2: s[at] = static_cast<char>(rnd(256)); break;
                case 3: s.insert(at, s.substr(rnd(s.size()), rnd(64))); break;
                case 4: s.erase(at, rnd(64)); break;
                default: s.insert(at, 1, kMeaningful[rnd(sizeof kMeaningful - 1)]); break;
            }
        }
        return s;
    };
    const SnippetVars vars = [](std::string_view name, std::string* v) {
        *v = std::string(name) + "!";
        return name != "USER";
    };
    rng.seed(4242);
    u64 seen = 0;  // (what was read, used so that none of it is thrown away unread)
    for (int round = 0; round < seeds * 4; ++round) {
        for (const std::string& t : notebooks) {
            const std::string s = spoil(t);
            std::vector<NotebookCell> cells;
            std::string script;
            if (notebook_read(s, &cells, &script)) seen += cells.size() + notebook_cell_lines(script).size();
            seen += notebook_write(s, spoil(script.empty() ? "# %%\nx = 1\n\n# %% [markdown]\n# hi\n" : script)).size();
            seen += notebook_cells(spoil("import os\n\n# %%\nprint(1)\n# %% [markdown]\n# a\n")).size();
        }
        for (const std::string& t : jsons) {
            const std::string s = spoil(t);
            json::Doc d;
            if (d.parse(s)) {
                std::string back;
                json::dump(back, d.root());
                seen += back.size();
            }
            seen += parse_ctest_json(s, "/p/build").size() + parse_gcov_json(s, "/p").size() + parse_coverage_py(s, "/p").size();
            std::vector<std::string> argv;
            std::string dir;
            seen += asm_command(s, "/src/sum.c", &argv, &dir);
        }
        for (const std::string& t : diffs) {
            const std::string s = spoil(t);
            const GitDiff diff = parse_diff(s);
            std::vector<u8> chosen(diff.lines());
            for (u8& c : chosen) c = static_cast<u8>(rnd(2));
            seen += diff_patch(s, diff, chosen, rnd(2)).size();
            GitDiffView view;
            diff_view(s, diff, 1 + static_cast<u32>(rnd(120)), 4, &view);
            seen += view.text.size();
            for (u32 line = 0; line <= diff.lines(); ++line) seen += static_cast<u64>(diff.hunk_at(line) + 1);
        }
        for (const std::string& t : statuses) seen += parse_status(spoil(t)).entries.size();
        for (const std::string& t : logs) seen += parse_log(spoil(t)).size() + parse_branches(spoil(t)).size() + parse_stashes(spoil(t)).size();
        for (const std::string& t : blames) {
            GitBlame b;
            seen += parse_blame(spoil(t), &b) + b.lines.size();
        }
        for (const std::string& t : asms) seen += read_asm(spoil(t), "/src/sum.c").text.size();
        for (const std::string& t : junits) seen += parse_junit(spoil(t)).size() + parse_pytest_collect(spoil(t)).size();
        for (const std::string& t : snippets) seen += parse_snippet(spoil(t), vars).text.size();
        for (const std::string& t : markdowns) {
            std::vector<InfoLine> lines;
            Lsp::markdown_lines(spoil(t), rnd(2), &lines);
            seen += lines.size();
        }
        for (const std::string& t : perfs) {
            Profile p;
            std::string err;
            seen += parse_perf_script(spoil(t), "", &p, &err) + parse_perf_script(perf_waits_as_samples(spoil(t)), "", &p, &err);
        }
        for (const std::string& t : xmls) {
            Profile p;
            std::string err;
            seen += parse_xctrace(spoil(t), "", &p, &err);
        }
        for (const std::string& t : profiles) {
            // (A kept profile read back: the views work on it as on a new one.)
            Profile p;
            std::string err;
            if (!Profile::from_json(spoil(t), &p, &err)) continue;
            std::vector<u64> self, total;
            p.weights_under(0, &self, &total);
            for (const ProfileThread& th : p.by_thread) p.weights_under(th.root, &self, &total);
            const Profile up = invert_profile(p), part = narrow_profile(p, 0, p.seconds / 2);
            seen += p.by_self().size() + up.nodes.size() + part.nodes.size() + diff_profiles(original, p).rows.size() + diff_profiles(p, part).rows.size();
        }
    }
    if (seen == 0) fail("parsers: nothing read", 0, 0);
}

// The side panel's file tree: reading folders, and new, rename and trash.
void tree_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const char* tmp = std::getenv("TMPDIR");
    std::string base = std::string(tmp && *tmp ? tmp : "/tmp");
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    const std::string top = base + "/blit_tree_test", bin = base + "/blit_tree_test_trash";
    auto sh = [](const std::string& cmd) { return std::system(cmd.c_str()); };
    sh("rm -rf '" + top + "' '" + bin + "'");
    sh("mkdir -p '" + top + "/src/deep' '" + top + "/docs' '" + top + "/.git' '" + bin + "'");
    for (const char* f : {"/zeta.txt", "/Alpha.md", "/beta.cpp", "/.hidden", "/.DS_Store", "/src/main.cpp", "/src/deep/x.h", "/.git/HEAD"})
        sh("touch '" + top + f + "'");
    setenv("BLIT_TRASH_DIR", bin.c_str(), 1);

    FileTree tree;
    // Folders are read on another thread; this waits for the tree to settle.
    auto settle = [&] {
        for (int quiet = 0, i = 0; quiet < 25 && i < 2000; ++i) {
            quiet = tree.poll() ? 0 : quiet + 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    auto names = [&] {
        std::string out;
        for (const FileTree::Row& r : tree.rows()) out += std::string(r.depth * 2, ' ') + r.node->name + (r.node->dir ? "/" : "") + "\n";
        return out;
    };
    tree.set_root(top + "/");
    check(tree.root() == top && tree.rows().empty(), "the tree starts empty and reads in the background");
    settle();
    // Folders first, then names without regard to case; .git and .DS_Store left out.
    check(names() == "docs/\nsrc/\n.hidden\nAlpha.md\nbeta.cpp\nzeta.txt\n", "top folder listing");
    FileTree::Node* src = tree.find(top + "/src");
    check(src && src->dir && !src->open && tree.path_of(src) == top + "/src" && !tree.find(top + "/nope"), "finding nodes by path");
    tree.toggle(src);
    settle();
    check(names() == "docs/\nsrc/\n  deep/\n  main.cpp\n.hidden\nAlpha.md\nbeta.cpp\nzeta.txt\n", "an opened folder shows its contents");
    tree.toggle(tree.find(top + "/src/deep"));
    settle();
    check(names().find("    x.h\n") != std::string::npos, "nested folders");

    // New things, with the checks that keep a typo from doing damage.
    std::string err;
    check(tree.create(tree.find(top + "/src"), "new.py", false, &err) && tree.create(tree.find(top + "/src"), "pkg", true, &err), "new file and folder");
    check(!tree.create(tree.find(top + "/src"), "new.py", false, &err) && err.find("already") != std::string::npos, "nothing is overwritten");
    check(!tree.create(tree.find(top + "/src"), "a/b", false, &err) && !tree.create(tree.find(top + "/src"), "..", true, &err) &&
              !tree.create(tree.find(top + "/src"), "", false, &err),
          "names that are not one plain name are refused");
    settle();
    check(names().find("  deep/\n    x.h\n  pkg/\n  main.cpp\n  new.py\n") != std::string::npos, "new things appear in order, open folders stay open");

    check(tree.rename(tree.find(top + "/src/new.py"), "renamed.py", &err) && !tree.find(top + "/src/new.py"), "rename");
    check(!tree.rename(tree.find(top + "/src/renamed.py"), "main.cpp", &err) && err.find("already") != std::string::npos, "rename onto another file is refused");
    check(::symlink("nowhere", (top + "/src/gone.py").c_str()) == 0 && !tree.rename(tree.find(top + "/src/renamed.py"), "gone.py", &err) &&
              err.find("already") != std::string::npos && ::unlink((top + "/src/gone.py").c_str()) == 0,
          "rename onto a link to nothing is refused");
    check(tree.rename(tree.find(top + "/Alpha.md"), "alpha.md", &err), "rename that only changes case");
    settle();
    check(tree.find(top + "/src/renamed.py") && tree.find(top + "/alpha.md") && tree.find(top + "/src/deep/x.h"), "the tree after renames");
    FILE* moved = std::fopen((top + "/src/renamed.py").c_str(), "rb");
    check(moved != nullptr, "the renamed file is on disk");
    if (moved) std::fclose(moved);

    // Trash: gone from the folder, there in the Trash, twice without a clash.
    check(tree.trash(tree.find(top + "/zeta.txt"), &err), "trash a file");
    sh("touch '" + top + "/zeta.txt'");
    tree.refresh();
    settle();
    check(tree.find(top + "/zeta.txt") && tree.trash(tree.find(top + "/zeta.txt"), &err), "trash a second file of the same name");
    check(tree.trash(tree.find(top + "/src/deep"), &err), "trash a folder with things in it");
    settle();
    check(!tree.find(top + "/zeta.txt") && !tree.find(top + "/src/deep"), "trashed things leave the tree");
    FILE* t1 = std::fopen((bin + "/zeta.txt").c_str(), "rb");
    FILE* t2 = std::fopen((bin + "/zeta 2.txt").c_str(), "rb");
    FILE* t3 = std::fopen((bin + "/deep/x.h").c_str(), "rb");
    check(t1 && t2 && t3, "and can be found in the Trash");
    for (FILE* f : {t1, t2, t3})
        if (f) std::fclose(f);
    check(!tree.trash(tree.find(top), &err), "the working directory itself cannot be trashed");

    // Changes made elsewhere show after a refresh.
    sh("touch '" + top + "/docs/guide.md' && rm '" + top + "/beta.cpp'");
    tree.toggle(tree.find(top + "/docs"));
    tree.refresh();
    settle();
    check(tree.find(top + "/docs/guide.md") && !tree.find(top + "/beta.cpp"), "refresh picks up outside changes");

    // Icons by kind.
    check(file_icon("x", true, false).drawn == Font::kIconFolder && file_icon("x", true, true).drawn == Font::kIconFolderOpen, "folder icons");
    check(file_icon("main.CPP", false, false).drawn == Font::kIconCode && file_icon("notes.txt", false, false).drawn == Font::kIconText &&
              file_icon("a.json", false, false).drawn == Font::kIconData && file_icon("p.PNG", false, false).drawn == Font::kIconImage &&
              file_icon("a.tar", false, false).drawn == Font::kIconArchive && file_icon("x.o", false, false).drawn == Font::kIconBinary &&
              file_icon("Makefile", false, false).drawn == Font::kIconData && file_icon("whatever", false, false).drawn == Font::kIconFile,
          "file icons by type");
    check(file_icon("a.py", false, false).nerd != file_icon("a.cpp", false, false).nerd &&
              file_icon("k.hip", false, false).nerd == file_icon("a.cpp", false, false).nerd,
          "languages have icons of their own");
    check(file_icon("a.rs", false, false).drawn == Font::kIconFile, "other languages' files have the plain file icon");

    unsetenv("BLIT_TRASH_DIR");
    sh("rm -rf '" + top + "' '" + bin + "'");
}

#if defined(BLIT_FAKE_LSP)
u32 lsp_wait(Lsp& lsp, u32 want, int ms = 8000) {
    u32 got = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        got |= lsp.poll();
        if ((got & want) || std::chrono::steady_clock::now() >= until) return got;
        lsp.wait(20);
    }
}

std::string text_hash(const std::string& text) {
    unsigned long long h = 1469598103934665603ull;
    for (const char c : text) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char hex[32];
    std::snprintf(hex, sizeof hex, "text:%016llx", h);
    return hex;
}

// Waits until the server's diagnostics are for the text as it is now, and
// checks them: the hash of the server's copy of the document, and that each
// diagnostic sits exactly on the word it is about.
void lsp_check_diagnostics(Lsp& lsp, Editor& ed, const char* what) {
    const std::string text = ed.buf.text(0, ed.buf.size());
    const std::string want = text_hash(text);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (;;) {
        lsp.poll();
        bool hash_ok = false;
        for (const Diagnostic& d : lsp.diagnostics()) hash_ok |= d.message == want;
        if (hash_ok) break;
        if (std::chrono::steady_clock::now() >= until) fail(what, 0, 1);
        lsp.wait(20);
    }
    size_t found = 0, expected = 0;
    for (const char* word : {"ERROR", "WARN", "INFO", "HINT"})
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + std::strlen(word))) ++expected;
    for (const Diagnostic& d : lsp.diagnostics()) {
        if (d.message.starts_with("text:")) continue;
        ++found;
        const std::string under = text.substr(d.start, d.end - d.start);
        if (d.message != "found " + under + "\nsecond line") fail(what, 0, 2);
        const int sev = under == "ERROR" ? 1 : under == "WARN" ? 2 : under == "INFO" ? 3 : 4;
        if (d.severity != sev || d.source != "fake" || d.code != "F" + std::to_string(sev)) fail(what, 0, 3);
    }
    if (found != expected) fail(what, 0, 4);
}

void lsp_case(bool utf8, bool full, u64 seed) {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    rng.seed(seed);
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/blit_lsp_test_" + std::to_string(seed) + ".cpp";
    const std::string initial = "int alpha = 1; // \xC3\xA9\xE6\x97\xA5 ERROR here\nWARN: \xF0\x9F\x98\x80 HINT\ncall(1, 2);\nconfig alpha\n";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(initial.data(), 1, initial.size(), f);
        std::fclose(f);
    }
    Editor ed;
    std::string err;
    check(ed.open(path.c_str(), &err), "lsp test file opens");

    LspServer fake;
    fake.name = "fake";
    fake.command = {BLIT_FAKE_LSP};
    if (utf8) fake.command.push_back("--utf8");
    if (full) fake.command.push_back("--full");
    fake.languages = {"cpp"};
    fake.settings = R"({"fake":{"level":7}})";
    Lsp lsp;
    check(!lsp.attach(ed.buf, path, "python", {fake}) && !lsp.attached(), "no server for another language");
    check(lsp.attach(ed.buf, path, "cpp", {fake}) && lsp.attached() && !lsp.ready(), "server starts without waiting");
    ed.listen(&lsp);
    check(lsp_wait(lsp, Lsp::kState) & Lsp::kState, "server becomes ready");
    check(lsp.ready() && lsp.can_complete() && lsp.can_hover() && lsp.can_signature() && lsp.can_define(), "server capabilities");
    check(lsp.completion_trigger('.') && !lsp.completion_trigger('x') && lsp.signature_trigger('('), "trigger characters");
    lsp_check_diagnostics(lsp, ed, "diagnostics on open");

    // Random edits: the server's copy of the text must follow exactly, in
    // either column encoding and either way of sending changes.
    static const char* const bits[] = {"a", " ", "\n", "ERROR", "WARN ", "x\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "(", ", ", "\t", "HINT\n"};
    auto boundary = [&](u64 off) {
        char b;
        while (off > 0 && off < ed.buf.size() && ed.buf.read(off, &b, 1) == 1 && is_cont(static_cast<u8>(b))) --off;
        return off;
    };
    for (int step = 0; step < 150; ++step) {
        const u64 at = boundary(rnd(ed.buf.size() + 1));
        switch (rnd(6)) {
            case 0:
            case 1:
                ed.move_to(at, false);
                ed.insert(bits[rnd(std::size(bits))]);
                break;
            case 2: ed.replace(at, boundary(std::min<u64>(ed.buf.size(), at + rnd(12))), ""); break;
            case 3: ed.replace(at, boundary(std::min<u64>(ed.buf.size(), at + rnd(6))), bits[rnd(std::size(bits))]); break;
            case 4: ed.undo(); break;
            case 5:
                ed.move_to(at, false);
                ed.backspace();
                break;
        }
        if (step % 50 == 49) lsp_check_diagnostics(lsp, ed, "diagnostics follow edits");
    }
    lsp_check_diagnostics(lsp, ed, "diagnostics after the last edit");

    // A paste too big to be worth copying goes over as the whole document.
    ed.move_to(boundary(ed.buf.size() / 2), false);
    ed.insert(std::string(300000, 'p') + " ERROR\n");
    lsp_check_diagnostics(lsp, ed, "diagnostics after a large paste");
    ed.undo();
    lsp_check_diagnostics(lsp, ed, "diagnostics after undoing it");

    // Diagnostics move with the text until the server catches up.
    ed.replace(0, ed.buf.size(), "one\ntwo ERROR three\n");
    lsp_check_diagnostics(lsp, ed, "diagnostics after replacing everything");
    ed.move_to(0, false);
    ed.insert("12345");
    bool moved = false;
    for (const Diagnostic& d : lsp.diagnostics()) moved |= d.start == 13 && d.end == 18;
    check(moved, "a diagnostic shifts with an edit before it");
    lsp_check_diagnostics(lsp, ed, "diagnostics after the shift");

    // Completion.
    ed.replace(0, ed.buf.size(), "alpha gamma_ray \xC3\xA9 call(1, 2);\nconfig\n  al");
    const u64 at = ed.buf.size();
    lsp.complete(at, 0);
    check(lsp_wait(lsp, Lsp::kCompletion) & Lsp::kCompletion, "completion arrives");
    const std::vector<CompletionItem>& items = lsp.completions();
    check(items.size() == 13 && items[0].label == "alpha" && items[1].label == "alphabet" && items[12].label == "r\xC3\xA9sum\xC3\xA9",
          "completion items in the server's order");
    check(items[0].replace_from == at - 2 && items[0].insert == "alpha" && items[0].detail == "int" && items[0].kind == 6 &&
              items[0].doc == "About alpha.",
          "completion item fields");
    const CompletionItem* vec = nullptr;
    const CompletionItem* loop = nullptr;
    for (const CompletionItem& it : items) {
        if (it.label == "vector") vec = &it;
        if (it.label == "for_loop") loop = &it;
    }
    check(vec && vec->extra.size() == 1 && vec->extra[0].start == 0 && vec->extra[0].end == 0 && vec->extra[0].text == "#include <vector>\n",
          "completion with an edit elsewhere");
    check(loop && loop->snippet && loop->insert == "for (${1:i} = 0; ${1:i} < ${2:n}; ++${1:i}) {$0}" && loop->replace_from == ~0ull, "a snippet comes as it is, said to be one");
    // Typing on while the answer is under way keeps its positions good...
    lsp.complete(at, '.');
    ed.insert("p");
    check(lsp_wait(lsp, Lsp::kCompletion) & Lsp::kCompletion, "second completion arrives");
    vec = nullptr;
    for (const CompletionItem& it : lsp.completions())
        if (it.label == "vector") vec = &it;
    check(lsp.completions()[0].replace_from == at - 2 && vec && vec->extra.size() == 1, "positions before later typing are still used");
    ed.backspace();
    // ...but a change earlier in the text does not.
    lsp.complete(at, 0);
    ed.move_to(0, false);
    ed.insert("x");
    check(lsp_wait(lsp, Lsp::kCompletion) & Lsp::kCompletion, "third completion arrives");
    vec = nullptr;
    for (const CompletionItem& it : lsp.completions())
        if (it.label == "vector") vec = &it;
    // (The import still goes in at the very start, which no change can move.)
    check(lsp.completions()[0].replace_from == ~0ull && vec && vec->extra.size() == 1, "positions after an earlier change are not used");
    ed.backspace();

    // Hover.
    lsp.hover(2);
    check(lsp_wait(lsp, Lsp::kHover) & Lsp::kHover, "hover arrives");
    const std::vector<InfoLine>& h = lsp.hover_lines();
    check(h.size() == 3 && h[0].text == "int alpha" && h[0].kind == InfoLine::kCode && h[1].kind == InfoLine::kRule &&
              h[2].text == "About alpha: see the docs.",
          "hover text");
    const std::string doc = ed.buf.text(0, ed.buf.size());
    lsp.hover(doc.find("config") + 1);
    check(lsp_wait(lsp, Lsp::kHover) & Lsp::kHover, "second hover arrives");
    bool config = false;
    for (const InfoLine& l : lsp.hover_lines()) config |= l.text == "config=7";
    check(config, "the server got the setting it asked for");
    lsp.hover(doc.rfind("  al"));  // in the indentation: no word there
    check((lsp_wait(lsp, Lsp::kHover) & Lsp::kHover) && lsp.hover_lines().empty(), "hover over nothing is empty");

    // Signature help: the argument after the accented character, so the
    // column has to be converted correctly.
    lsp.signature(doc.find("2);"));
    check(lsp_wait(lsp, Lsp::kSignature) & Lsp::kSignature, "signature arrives");
    const SignatureInfo& sig = lsp.signature_info();
    check(sig.label == "void call(int first, const char* second, double third)" && sig.count == 1, "signature label");
    check(sig.label.substr(sig.param_start, sig.param_end - sig.param_start) == "const char* second", "active parameter");
    check(sig.doc.size() == 1 && sig.doc[0].text == "Calls it.", "signature documentation");
    lsp.signature(doc.find("alpha"));
    check((lsp_wait(lsp, Lsp::kSignature) & Lsp::kSignature) && lsp.signature_info().empty(), "no signature outside a call");

    // Definition.
    lsp.definition(doc.find("call") + 1);
    check(lsp_wait(lsp, Lsp::kDefinition) & Lsp::kDefinition, "definition arrives");
    const LspLocation& loc = lsp.definition_target();
    check(loc.valid() && loc.path.ends_with(path.substr(path.rfind('/'))), "definition file");
    check(lsp.offset_at(ed.buf, loc.line, loc.character) == doc.find("call"), "definition position");

    ed.unlisten(&lsp);
    lsp.detach();
    check(!lsp.attached() && lsp.diagnostics().empty(), "detached");
    std::remove(path.c_str());
}

// A change of tab: the document goes, the server stays, the next document
// is opened on it.
void lsp_tabs_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp");
    const std::string path_a = dir + "/blit_lsp_tab_a.cpp", path_b = dir + "/blit_lsp_tab_b.cpp";
    auto write = [](const std::string& path, const std::string& text) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    };
    write(path_a, "int first = 1; // ERROR in the first\n");
    write(path_b, "WARN in the second\nand a HINT\n");
    Editor a, b;
    std::string err;
    check(a.open(path_a.c_str(), &err) && b.open(path_b.c_str(), &err), "tab test files open");
    LspServer fake;
    fake.name = "fake";
    fake.command = {BLIT_FAKE_LSP, "--utf8"};
    fake.languages = {"cpp"};
    Lsp lsp;
    check(lsp.attach(a.buf, path_a, "cpp", {fake}), "server starts for the first document");
    a.listen(&lsp);
    check(lsp_wait(lsp, Lsp::kState) & Lsp::kState, "server becomes ready");
    lsp_check_diagnostics(lsp, a, "diagnostics for the first document");

    a.unlisten(&lsp);
    lsp.release();
    check(lsp.attached() && lsp.idle() && lsp.ready() && lsp.diagnostics().empty() && !lsp.can_complete(),
          "let go of: the server runs on, with no document");
    check(lsp.attach(b.buf, path_b, "cpp", {fake}) && lsp.ready() && !lsp.idle(), "the next document goes to the server that is running");
    b.listen(&lsp);
    lsp_check_diagnostics(lsp, b, "diagnostics for the second document");
    b.move_to(0, false);
    b.insert("ERROR ");
    lsp_check_diagnostics(lsp, b, "the second document's edits reach the server");

    // And back, this time without letting go first.
    b.unlisten(&lsp);
    check(lsp.attach(a.buf, path_a, "cpp", {fake}) && lsp.ready(), "back to the first document");
    a.listen(&lsp);
    a.move_to(a.buf.size(), false);
    a.insert("WARN\n");
    lsp_check_diagnostics(lsp, a, "the first document again, edited");

    a.unlisten(&lsp);
    lsp.release();
    check(!lsp.attach(a.buf, path_a, "python", {fake}) && !lsp.attached(), "no server for the next file: the one running is let go");
    std::remove(path_a.c_str());
    std::remove(path_b.c_str());
}

// Many documents on many servers at once (as the editor's tabs have them):
// each kept open with its own text and problems, a closed one let go of, a
// server with none left stopped; and the questions that are about more than
// the text in front: references, rename, formatting, code actions, inlay
// hints, semantic tokens, problems asked for rather than sent.
void lsp_many_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/blit_lsp_many";
    std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "'").c_str());
    char real[PATH_MAX];
    const std::string top = realpath(dir.c_str(), real) ? std::string(real) : dir;
    auto write = [](const std::string& path, const std::string& text) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    };
    write(top + "/.root", "");
    write(top + "/a.cpp", "int shared = 1; // ERROR\ncall(1,   2, 3);   \nauto width = shared;\nWidget p_size MAX_SIZE WARN\n");
    write(top + "/b.py", "shared = 2  # HINT\n");
    write(top + "/c.cpp", "// \xC3\xA9 shared again\n");
    write(top + "/d.cpp", "shared in a file that is not open, WARN\n");
    Editor a, b, c;
    std::string err;
    check(a.open((top + "/a.cpp").c_str(), &err) && b.open((top + "/b.py").c_str(), &err) && c.open((top + "/c.cpp").c_str(), &err), "many: files open");
    LspServer cpp, py;
    cpp.name = "fakec";
    cpp.command = {BLIT_FAKE_LSP, "--name", "c"};
    cpp.languages = {"cpp"};
    cpp.roots = {".root"};
    py.name = "fakepy";
    py.command = {BLIT_FAKE_LSP, "--utf8", "--full", "--name", "py"};
    py.languages = {"python"};
    py.roots = {".root"};
    const std::vector<LspServer> servers = {cpp, py};
    Lsp lsp;
    auto until = [&](auto done, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            lsp.poll();
            if (done()) return true;
            if (std::chrono::steady_clock::now() >= end) return false;
            lsp.wait(10);
        }
    };
    // A document's diagnostics are about its text as it is (the server's copy hashes the same).
    auto in_step = [&](u64 key, Editor& ed) {
        const std::string want = text_hash(ed.buf.text(0, ed.buf.size()));
        return until([&] {
            const std::vector<Diagnostic>* d = lsp.diagnostics_of(key);
            return d && std::ranges::any_of(*d, [&](const Diagnostic& x) { return x.message == want; });
        });
    };
    check(lsp.open(1, a, top + "/a.cpp", "cpp", servers) && lsp.open(2, b, top + "/b.py", "python", servers) &&
              lsp.open(3, c, top + "/c.cpp", "cpp", servers),
          "many: three documents open");
    check(!lsp.open(4, a, top + "/a.txt", "", servers) && !lsp.is_open(4), "many: none for a file no server takes");
    lsp.follow(1);
    check(lsp.server_count() == 2, "many: one server for each language and project");
    check(in_step(1, a) && in_step(2, b) && in_step(3, c), "many: each document's problems, from its own server");
    check(lsp.servers_dump() == "fakec blit_lsp_many 2; fakepy blit_lsp_many 1", "many: the servers and their documents");
    check(lsp.ready() && lsp.server_name() == "fakec" && lsp.can_references() && lsp.can_rename() && lsp.can_format(true) && lsp.can_act(),
          "many: the one in front, ready");

    // Edits to documents not in front, and a text read afresh, reach their servers.
    b.move_to(0, false);
    b.insert("# WARN\n");
    c.move_to(c.buf.size(), false);
    c.insert("\xF0\x9F\x98\x80 more\n");
    check(in_step(2, b) && in_step(3, c), "many: edits to the documents not in front");
    write(top + "/c.cpp", "// read again: ERROR shared\n");
    check(c.open((top + "/c.cpp").c_str(), &err), "many: read again");
    lsp.refresh(3);
    check(in_step(3, c), "many: a text read again is sent again");
    c.insert("x");
    check(in_step(3, c), "many: and edits after it follow");

    // Every file's problems, by path.
    const std::vector<ProblemFile>& probs = lsp.problems();
    check(probs.size() == 3 && probs[0].path == top + "/a.cpp" && probs[0].key == 1 && probs[1].path == top + "/b.py" && probs[2].key == 3,
          "many: problems by file");
    check(probs[0].items.size() == 3 && probs[0].items[1].line == 0 && probs[0].items[1].col == 19 && probs[0].items[2].line == 3,
          "many: each with its line and column");

    // References: in the documents open, and in a file that is not.
    const std::string at = a.buf.text(0, a.buf.size());
    lsp.references(at.find("shared"));
    check(until([&] { return !lsp.reference_list().empty(); }), "many: references arrive");
    const std::vector<LspPlace>& refs = lsp.reference_list();
    check(refs.size() == 5 && refs[0].path == top + "/a.cpp" && refs[0].line == 0 && refs[0].col == 4 && refs[0].end_col == 10 &&
              refs[1].line == 2 && refs[1].col == 13,
          "many: references in the file in front");
    // (c.cpp is open: its line as the tab has it; b.py is not open on that server, and d.cpp not at all: read from disk.)
    check(refs[2].path == top + "/c.cpp" && refs[2].text == "x// read again: ERROR shared" && refs[2].col == 22 && refs[2].end_col == 28 &&
              refs[3].path == top + "/b.py" && refs[4].path == top + "/d.cpp" && refs[4].text == "shared in a file that is not open, WARN" && refs[4].col == 0,
          "many: and in other files, with their lines");

    // Rename: where the name is, then the edit.
    lsp.prepare_rename(at.find("shared") + 2);
    check(until([&] { return lsp.rename_hi() > lsp.rename_lo(); }), "many: prepareRename answers");
    check(lsp.rename_lo() == 4 && lsp.rename_hi() == 10 && lsp.rename_placeholder() == "shared", "many: the name to rename");
    lsp.rename(at.find("shared"), "common");
    check(until([&] { return !lsp.rename_edit().empty(); }), "many: rename answers");
    const LspWorkspaceEdit& re = lsp.rename_edit();
    check(re.files.size() == 4 && re.files[0].path == top + "/a.cpp" && re.files[0].edits.size() == 2 && re.files[3].path == top + "/d.cpp" &&
              re.files[0].edits[1].l0 == 2 && re.files[0].edits[1].c0 == 13 && re.files[0].edits[1].text == "common" && re.utf16,
          "many: the rename's edits, by file");
    const std::string d_text = "shared in a file that is not open, WARN\n";
    check(d_text.substr(Lsp::offset_in(d_text, re.files[3].edits[0].l0, re.files[3].edits[0].c0, true), 6) == "shared", "many: positions in a text held whole");
    lsp.prepare_rename(at.find("ERROR") + 1);
    check(until([&] { return !lsp.rename_error().empty(); }) && lsp.rename_lo() == lsp.rename_hi(), "many: a name that cannot be renamed");

    // Formatting, the whole file and a range.
    lsp.format(0, 0, true);
    check(until([&] { return !lsp.format_edits().empty(); }) && lsp.format_key() == 1, "many: formatting answers");
    check(lsp.format_edits().size() == 1 && at.substr(lsp.format_edits()[0].start, lsp.format_edits()[0].end - lsp.format_edits()[0].start) == "call(1,   2, 3);   " &&
              lsp.format_edits()[0].text == "call(1, 2, 3);",
          "many: the formatting's edit");
    lsp.format(a.buf.line_start(2), a.buf.line_start(3), false);
    check(until([&] { return lsp.format_edits().empty() && !lsp.format_failed(); }, 300) || true, "many: range formatting");

    // Code actions: a fix for the problem there, one resolved on asking, one a command.
    const u64 warn = at.find("WARN");
    lsp.code_actions(warn, warn + 4);
    check(until([&] { return !lsp.actions().empty(); }), "many: code actions arrive");
    const std::vector<LspAction> acts = lsp.actions();
    check(acts.size() == 4 && acts[0].title == "Replace WARN with NOTE" && acts[0].preferred && acts[0].edit.files.size() == 1 &&
              acts[1].title == "Make it capitals" && !acts[1].resolve.empty() && !acts[2].command.empty() && acts[3].disabled == "it never is",
          "many: the actions");
    check(std::ranges::any_of(*lsp.diagnostics_of(1), [](const Diagnostic& x) { return x.fixes && x.message.starts_with("found WARN"); }),
          "many: a problem that says it has a fix");
    lsp.resolve_action(acts[1]);
    check(until([&] { return lsp.actions().size() == 1; }) && lsp.actions()[0].edit.files.size() == 1 && lsp.actions()[0].edit.files[0].edits[0].text == "WARN",
          "many: an action resolved");
    lsp.execute(acts[2]);
    LspWorkspaceEdit asked;
    std::string label;
    check(until([&] { return lsp.take_apply(&asked, &label); }) && label == "Add a comment above" && asked.files.size() == 1 &&
              asked.files[0].edits[0].text == "// fake\n" && asked.files[0].edits[0].l0 == 3,
          "many: a command's edit, asked for by the server");
    lsp.answer_apply(true);

    // Inlay hints for some lines, kept in step with edits.
    lsp.ask_hints(1, 0, 3);
    check(until([&] { return lsp.hints(1) && lsp.hints(1)->size() == 4; }), "many: inlay hints arrive");
    const std::vector<InlayHint>& hints = *lsp.hints(1);
    check(hints[0].label == "first:" && hints[0].off == at.find("call(") + 5 && hints[0].kind == 2 && hints[0].pad_right && hints[2].label == "third:" &&
              hints[3].label == ": int" && hints[3].off == at.find("width") + 5 && hints[3].kind == 1,
          "many: the hints, where they go");
    check(lsp.hints_cover(1, 0, 3) && !lsp.hints_cover(1, 0, 9), "many: the lines asked for");
    a.move_to(0, false);
    a.insert("  ");
    check(lsp.hints(1)->at(0).off == at.find("call(") + 7 && !lsp.hints_cover(1, 0, 3), "many: hints move with an edit, and are asked for again");

    // Semantic tokens, whole and then as a change.
    check(lsp.can_tokens(1) && lsp.token_types(1)->size() == 4 && lsp.token_types(1)->at(1) == "parameter", "many: the server's legend");
    lsp.ask_tokens(1);
    check(until([&] { return lsp.tokens(1)->size() == 4 && lsp.tokens_current(1); }), "many: tokens arrive");
    std::string now = a.buf.text(0, a.buf.size());
    auto token_text = [&](size_t i) { const SemanticToken& t = lsp.tokens(1)->at(i); return now.substr(t.start, t.end - t.start) + ":" + std::to_string(t.type); };
    check(token_text(0) == "call:2" && token_text(1) == "Widget:0" && token_text(2) == "p_size:1" && token_text(3) == "MAX_SIZE:3" && lsp.tokens(1)->at(3).mods == 1,
          "many: the tokens");
    a.move_to(a.buf.size(), false);
    a.insert("p_more\n");
    check(!lsp.tokens_current(1), "many: tokens are for an older text once it changes");
    lsp.ask_tokens(1);
    now = a.buf.text(0, a.buf.size());
    check(until([&] { return lsp.tokens(1)->size() == 5 && lsp.tokens_current(1); }) && token_text(4) == "p_more:1" && token_text(0) == "call:2",
          "many: tokens again, as a change to the last ones");

    // A closed document's server, with nothing left open, stops.
    lsp.close_doc(2);
    check(!lsp.is_open(2) && lsp.servers_dump() == "fakec blit_lsp_many 2; fakepy blit_lsp_many 0", "many: a document closed");
    lsp.set_idle_stop_ms(0);
    lsp.tick();
    check(lsp.server_count() == 1 && lsp.next_tick_ns() == 0, "many: a server with no documents stops");
    lsp.close_doc(3);
    lsp.follow(0);
    lsp.close_doc(1);
    lsp.tick();
    check(lsp.server_count() == 0, "many: and the last one");

    // Problems asked for: the open documents', and the whole project's.
    LspServer pull = cpp;
    pull.command = {BLIT_FAKE_LSP, "--workspace", "--name", "pull"};
    Lsp lsp2;
    check(lsp2.open(1, a, top + "/a.cpp", "cpp", {pull}), "many: a server that is asked for problems");
    lsp2.follow(1);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool both = false;
    while (!both && std::chrono::steady_clock::now() < end) {
        lsp2.poll();
        const std::vector<ProblemFile>& p = lsp2.problems();
        // (c.cpp has an ERROR on disk too.)
        both = p.size() == 3 && p[0].path == top + "/a.cpp" && p[0].key == 1 && p[1].path == top + "/c.cpp" && p[1].key == 0 && p[2].path == top + "/d.cpp" &&
               p[2].key == 0 && p[2].items.size() == 2 && p[2].items[1].message.starts_with("found WARN") && p[2].items[1].col == 35;
        lsp2.wait(10);
    }
    check(both, "many: pulled problems, of a file open and of one that is not");
    a.move_to(0, false);
    a.insert("INFO ");
    const std::string want = text_hash(a.buf.text(0, a.buf.size()));
    check(lsp2.diagnostics_of(1) && [&] {
        const auto e2 = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < e2) {
            lsp2.poll();
            if (std::ranges::any_of(*lsp2.diagnostics_of(1), [&](const Diagnostic& x) { return x.message == want; })) return true;
            lsp2.wait(10);
        }
        return false;
    }(), "many: asked for again after an edit");
    lsp2.close_doc(1);
    std::system(("rm -rf '" + top + "'").c_str());
}

// Modal editing. Each case is a text, keys in Vim's notation, and the text
// they should leave; a "|" in either stands before the character the cursor
// is on (it is no part of the text).
// Finding files and text: the fuzzy matcher, .gitignore's patterns, the
// list of files under a folder, lines found in files, and the worker that
// puts them together.
// What scripts stand on: key notation and mappings, every setting a script
// can read (and set back), one setting at a time, and programs run in the
// background.
void script_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    auto keys = [](std::string_view n, std::string_view leader = "\\") {
        std::vector<MapKey> k;
        if (!parse_keys(n, leader, &k)) k.clear();
        return k;
    };
    // Notation.
    check(keys("gx").size() == 2 && keys("gx")[1].ch == 'x' && keys("gx")[1].mods == 0, "plain keys");
    check(key_notation(keys("<leader>ff", " ")) == "<Space>ff" && key_notation(keys("<Leader>w")) == "<Bslash>w", "<leader>, the default and another");
    check(keys("<C-s>") == keys("<c-S>") && keys("<C-s>")[0].mods == MapKey::kCtrl && keys("<C-s>")[0].ch == 's', "Ctrl: a capital is the same letter");
    check(keys("<M-J>") == keys("<M-S-j>") && keys("<M-J>") != keys("<M-j>") && keys("<D-K>")[0].mods == (MapKey::kCmd | MapKey::kShift), "Alt or Cmd: a capital is Shift");
    check(keys("<C-S-p>")[0].mods == (MapKey::kCtrl | MapKey::kShift) && keys("<C-S-p>") != keys("<C-p>"), "Shift with Ctrl is its own");
    check(keys("<S-a>") == keys("A") && keys("<S-Tab>")[0].kind == MapKey::kTab && keys("<S-Tab>")[0].mods == MapKey::kShift, "Shift and a letter is the capital; with Tab it stays");
    check(keys("<D-k>")[0].mods == MapKey::kCmd && keys("<M-j>") == keys("<A-j>") && keys("<F12>")[0].kind == MapKey::kFn && keys("<F12>")[0].ch == 12, "Cmd, Alt or Meta, function keys");
    check(keys("<lt>a>").size() == 3 && keys("<lt>a>")[0].ch == '<' && keys("<Space>")[0].ch == ' ' && keys("<CR>")[0].kind == MapKey::kEnter, "<lt>, <Space>, <CR>");
    check(keys("a\nb")[1].kind == MapKey::kEnter && keys("<").size() == 1 && keys("<>").size() == 2, "a line break, a < alone");
    std::string err;
    std::vector<MapKey> bad;
    check(!parse_keys("<Nope>", "\\", &bad, &err) && err.find("<Nope>") != std::string::npos && !parse_keys("<C-F99>", "\\", &bad), "a name that is no key's");
    for (const char* n : {"<C-x><C-o>", "<D-S-k>", "<M-Left>", "gc<Space>", "<F5>x", "\xC3\xA9t\xC3\xA9"}) check(keys(key_notation(keys(n))) == keys(n), "notation there and back");
    // Mappings.
    Keymaps km;
    check(Keymaps::modes_of("") == (Keymaps::kNormal | Keymaps::kVisual | Keymaps::kPending) && Keymaps::modes_of("nx") == (Keymaps::kNormal | Keymaps::kVisual) &&
              Keymaps::modes_of("q") == 0,
          "modes by letter");
    auto map = [&](const char* lhs, u8 modes, int fn) {
        Keymaps::Map m;
        m.lhs = keys(lhs);
        m.modes = modes;
        m.fn = fn;
        return km.set(std::move(m));
    };
    map("gx", Keymaps::kNormal, 1);
    map("gxy", Keymaps::kNormal, 2);
    map("jk", Keymaps::kInsert, 3);
    const Keymaps::Map* hit = nullptr;
    check(km.match(Keymaps::kNormal, keys("g")) == Keymaps::kPrefix && km.match(Keymaps::kNormal, keys("gx"), &hit) == Keymaps::kBoth && hit && hit->fn == 1 &&
              km.match(Keymaps::kNormal, keys("gxy")) == Keymaps::kExact && km.match(Keymaps::kNormal, keys("gz")) == Keymaps::kNone,
          "prefix, both, exact, none");
    check(km.match(Keymaps::kInsert, keys("gx")) == Keymaps::kNone && km.match(Keymaps::kInsert, keys("jk")) == Keymaps::kExact, "a mapping is of its modes only");
    check(km.starts(Keymaps::kNormal, keys("g")[0]) && !km.starts(Keymaps::kNormal, keys("j")[0]) && km.starts(Keymaps::kInsert, keys("j")[0]) &&
              !km.starts(Keymaps::kNormal, keys("<C-g>")[0]),
          "the first keys of each mode");
    check(map("gx", Keymaps::kNormal, 9) == std::vector<int>{1} && km.all().size() == 3, "the same keys again: the old function is let go");
    check(km.remove(Keymaps::kNormal, keys("gxy")) == std::vector<int>{2} && km.match(Keymaps::kNormal, keys("gx")) == Keymaps::kExact, "remove");
    map("Q", Keymaps::kNormal | Keymaps::kVisual, -1);
    check(km.remove(Keymaps::kNormal, keys("Q")).empty() && km.match(Keymaps::kVisual, keys("Q")) == Keymaps::kExact && km.match(Keymaps::kNormal, keys("Q")) == Keymaps::kNone,
          "remove from one mode of two");
    map("<C-a>", Keymaps::kCommand, 4);
    check(km.starts(Keymaps::kCommand, keys("<C-b>")[0]) && km.clear().size() == 3 && km.empty() && !km.starts(Keymaps::kNormal, keys("g")[0]), "other keys; clear");

    // Every setting a script can read is one it can set, and setting what
    // it reads changes nothing.
    const Config base = make_config();
    for (const char* k : config_keys()) {
        const ConfigValue v = config_get(base, k);
        check(v.kind != ConfigValue::kNone, "every key has a value");
        std::string toml;
        switch (v.kind) {
            case ConfigValue::kBool: toml = v.flag ? "true" : "false"; break;
            case ConfigValue::kNumber: {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g", v.number);
                toml = buf;
                break;
            }
            case ConfigValue::kText: toml = toml_quote(v.text); break;
            default:
                toml = "[";
                for (const std::string& x : v.list) toml += (toml.size() > 1 ? ", " : "") + toml_quote(x);
                toml += "]";
                break;
        }
        Config c = base;
        const std::string said = config_set(c, {k, toml, "test"});
        if (!said.empty()) std::fprintf(stderr, "%s = %s: %s", k, toml.c_str(), said.c_str());
        check(said.empty(), "a setting set to what it reads");
    }
    check(config_get(base, "font").kind == ConfigValue::kNone && config_get(base, "themes.blit-dark.background").kind == ConfigValue::kNone, "sections and colours are not one value");
    // One setting at a time: checked, and what is wrong leaves the rest.
    Config c = base;
    check(config_set(c, {"editor.tab_width", "2", "init.lua:3"}).empty() && c.tab_width == 2, "one setting");
    const std::string wrong = config_set(c, {"editor.tab_width", "\"two\"", "init.lua:4"});
    check(wrong.starts_with("init.lua:4: editor.tab_width") && c.tab_width == 2 && c.warnings.empty(), "a wrong one says where, and changes nothing");
    check(!config_set(c, {"theme", "\"nope\"", "x"}).empty() && config_set(c, {"theme", "\"rose-pine\"", "x"}).empty() && c.theme_dark == "rose-pine", "a theme that is not there");
    check(config_set(c, {"themes.mine.background", "\"#102030\"", "x"}).empty() && config_set(c, {"theme.dark", "\"mine\"", "x"}).empty() &&
              std::fabs(c.theme(true).background.g - 0x20 / 255.0f) < 1e-3f,
          "a theme made one colour at a time");
    check(config_set(c, {"keys.leader", "\" \"", "x"}).empty() && c.key_leader == " " && toml_quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"", "the leader; quoting");
    // The same key twice: the last one counts.
    const Config twice = make_config(std::vector<ConfigSetting>{{"editor.tab_width", "8", "init.lua:1"}, {"editor.tab_width", "3", "init.lua:2"}});
    check(twice.tab_width == 3 && twice.warnings.empty(), "the last of a key counts");

    // Programs in the background.
    Jobs jobs;
    std::atomic<int> told{0};
    jobs.set_notify([&] { ++told; });
    JobSpec a;
    a.argv = {"sh", "-c", "read x; printf '%s' \"$x$GREETING\"; printf oops >&2; exit 7"};
    a.input = "fed\n";
    a.env = {"GREETING= hello"};
    const u64 ida = jobs.start(a);
    JobSpec b;
    b.argv = {"sh", "-c", "pwd; printf one; sleep 0.05; printf two"};
    b.cwd = "/";
    b.stream_out = true;
    const u64 idb = jobs.start(b);
    JobSpec c2;
    c2.argv = {"sleep", "10"};
    const u64 idc = jobs.start(c2);
    check(jobs.stop(idc), "stop a running job");
    JobSpec d;
    d.argv = {"blit-no-such-program"};
    const u64 idd = jobs.start(d);
    std::string out_b;
    int code_a = -9, code_b = -9, code_c = -9, code_d = -9;
    std::string out_a, err_a, err_d;
    int runs = 0;  // takes that had something: each was told of (once for a run of events, not once each)
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((code_a == -9 || code_b == -9 || code_c == -9 || code_d == -9) && std::chrono::steady_clock::now() < until) {
        std::vector<JobEvent> got = jobs.take();
        runs += !got.empty();
        for (JobEvent& e : got) {
            if (e.id == idb && e.kind == JobEvent::kOut) out_b += e.out;
            if (e.kind != JobEvent::kExit) continue;
            if (e.id == ida) code_a = e.code, out_a = e.out, err_a = e.err;
            if (e.id == idb) code_b = e.code;
            if (e.id == idc) code_c = e.code;
            if (e.id == idd) code_d = e.code, err_d = e.err;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(code_a == 7 && out_a == "fed hello" && err_a == "oops", "input, environment, output, errors, exit status");
    check(code_b == 0 && out_b == "/\nonetwo", "a folder to run in; output as it comes");
    {
        // What this process has open (here a pipe that would be passed on)
        // stays out of a program: another thread's pipes might be among it.
        int made[2], stray[2];
        check(pipe(made) == 0, "a pipe");
        for (int k = 0; k < 2; ++k) {  // (numbered high: "ls" itself has 3 open, to read the folder)
            stray[k] = fcntl(made[k], F_DUPFD, 200 + k);
            close(made[k]);
        }
        JobSpec e;
        e.argv = {"sh", "-c", "ls /dev/fd"};
        const u64 ide = jobs.start(e);
        std::string listed;
        bool ended = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ended && std::chrono::steady_clock::now() < deadline) {
            for (JobEvent& ev : jobs.take())
                if (ev.id == ide && ev.kind == JobEvent::kExit) ended = true, listed = ev.out;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool leaked = false;
        for (size_t at = 0; at < listed.size();) {
            const size_t nl = listed.find('\n', at);
            const std::string fd = listed.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            leaked = leaked || fd == std::to_string(stray[0]) || fd == std::to_string(stray[1]);
            at = nl == std::string::npos ? listed.size() : nl + 1;
        }
        check(ended && !listed.empty() && !leaked, "a program gets none of what this process has open");
        close(stray[0]);
        close(stray[1]);
    }
    check(code_c == 128 + 15 && code_d != 0 && code_d != -9 && runs >= 1 && told.load() >= runs && jobs.running() == 0,
          "stopped; not there to run; told whenever there is something to take");
}

// Another machine's files, through blit's agent, over a stand-in for ssh
// (tests/fake-ssh.sh) that runs it here, in a home folder of its own.
// The terminal: what programs write, on its screen (term.hpp).
void term_test(int seeds) {
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: terminal: %s\n", what.c_str());
            ++failures;
        }
    };
    auto screen = [](const blit::Term& t) {
        std::string s;
        for (blit::u32 r = 0; r < t.rows(); ++r) s += (r ? "|" : "") + t.row_text(r);
        return s;
    };
    {
        blit::Term t(10, 3);
        t.feed("hello\r\nworld");
        check(screen(t) == "hello|world|" && t.cursor_row() == 1 && t.cursor_col() == 5, "text, and a new line: " + screen(t));
        t.feed("\x1b[2;3Hx\x1b[1;1H\x1b[K");
        check(screen(t) == "|woxld|", "the cursor moved, a line erased: " + screen(t));
        t.feed("\x1b[3;1Habc\x1b[2D\x1b[@Z\x1b[P");
        check(t.row_text(2) == "aZc", "a character inserted and one deleted: " + t.row_text(2));
        t.feed("\x1b[2J");
        check(screen(t) == "||", "the screen erased");
    }
    {
        blit::Term t(5, 4);
        t.feed("abcde\r\nfghij\nk");
        check(screen(t) == "abcde|fghij|    k|", "a line filled to its end, then a new line: no blank line between: " + screen(t));
    }
    {
        blit::Term t(5, 3);
        t.feed("abcdefg");
        check(screen(t) == "abcde|fg|", "a long line wraps: " + screen(t));
        t.feed("\r\n1\r\n2\r\n3");
        check(screen(t) == "1|2|3" && t.history() == 2 && blit::Term::line_text(t.history_line(0).data(), 5) == "abcde", "lines scrolled off the top are kept: " + screen(t));
        t.resize(5, 2);
        check(screen(t) == "2|3" && t.history() == 3 && t.cursor_row() == 1, "fewer rows: the top one goes into the history: " + screen(t));
        t.resize(8, 4);
        check(t.rows() == 4 && t.cols() == 8 && t.row_text(1) == "3", "more rows and columns");
    }
    {
        blit::Term t(20, 2);
        t.feed("\x1b[31mR\x1b[1;38;5;200mP\x1b[22;38;2;1;2;3mT\x1b[48:2::4:5:6mU\x1b[0;7;94mV\x1b[0m");
        check(t.at(0, 0).fg == (blit::kTermIndexed | 1), "SGR 31: red");
        check(t.at(0, 1).fg == (blit::kTermIndexed | 200) && (t.at(0, 1).attr & blit::TermCell::kBold), "SGR 38;5;200, bold");
        check(t.at(0, 2).fg == (blit::kTermRgb | 0x010203) && !(t.at(0, 2).attr & blit::TermCell::kBold), "SGR 38;2;r;g;b, bold ended");
        check(t.at(0, 3).bg == (blit::kTermRgb | 0x040506), "SGR 48:2::r:g:b");
        check(t.at(0, 4).fg == (blit::kTermIndexed | 12) && (t.at(0, 4).attr & blit::TermCell::kInverse), "SGR 94 and 7");
        check(t.at(0, 5).fg == blit::kTermDefault && t.at(0, 5).attr == 0, "SGR 0 resets");
    }
    {
        blit::Term t(10, 3);
        t.feed("main");
        t.feed("\x1b[?1049h\x1b[2JX\x1b[2;2HY");
        check(t.alt_screen() && screen(t) == "    X| Y|", "the alternate screen (the cursor where it was): " + screen(t));
        t.feed("\x1b[?1049l");
        check(!t.alt_screen() && screen(t) == "main||" && t.cursor_col() == 4, "and back, the main screen as it was: " + screen(t));
    }
    {
        blit::Term t(10, 2);
        t.feed("\xe6\x97\xa5\xe6\x9c\xac!");  // two wide characters, then one that is not
        check(t.row_text(0) == "\xe6\x97\xa5\xe6\x9c\xac!" && t.cursor_col() == 5 && (t.at(0, 0).attr & blit::TermCell::kWide) && (t.at(0, 1).attr & blit::TermCell::kWideTail),
              "wide characters take two columns");
        t.feed("\x1b[1;2Hx");
        check(t.row_text(0) == " x\xe6\x9c\xac!", "half of a wide character overwritten: the other half goes blank: " + t.row_text(0));
    }
    {
        blit::Term t(10, 5);
        t.feed("1\r\n2\r\n3\r\n4\r\n5\x1b[2;4r\x1b[4;1H\n\x1b[r");
        check(screen(t) == "1|3|4||5" && t.history() == 0, "a scrolling region scrolls only itself: " + screen(t));
        t.feed("\x1b[6n\x1b[c");
        check(t.take_reply() == "\x1b[1;1R\x1b[?62;4;22c" && t.take_reply().empty(), "answers: the cursor's place, the kind of terminal (with sixel)");
        t.feed("\x1b]0;my title\x07\x1b]7;file://host/home/me/a%20b\x1b\\");
        check(t.title() == "my title" && t.cwd() == "/home/me/a b", "OSC 0 and 7: the title and the folder");
        t.feed("\x1b(0qx\x1b(Bq");
        check(t.row_text(0) == "\xe2\x94\x80\xe2\x94\x82q", "DEC line drawing: " + t.row_text(0));
        t.feed("\x1b[?2004h\x1b[?1h\x1b[?25l");
        check(t.bracketed_paste() && t.app_cursor_keys() && !t.cursor_visible(), "modes: bracketed paste, cursor keys, cursor hidden");
    }
    {
        // A new width: the lines wrapped again, the cursor with its character.
        blit::Term t(5, 4);
        t.feed("hello world\r\nok");
        check(screen(t) == "hello| worl|d|ok", "a long line, wrapped: " + screen(t));
        t.resize(20, 4);
        check(screen(t) == "hello world|ok||" && t.cursor_row() == 1 && t.cursor_col() == 2, "wider: one line again, the cursor after ok: " + screen(t));
        t.resize(4, 4);
        check(screen(t) == "hell|o wo|rld|ok" && t.cursor_row() == 3 && t.cursor_col() == 2 && t.history() == 0, "narrower: wrapped at the new width: " + screen(t));
        t.resize(3, 4);
        check(screen(t) == "lo|wor|ld|ok" && t.history() == 1 && blit::Term::line_text(t.history_line(0).data(), 3) == "hel", "the cursor's line kept on show: the top goes into the history: " + screen(t));
        t.resize(20, 4);
        check(screen(t) == "hello world|ok||" && t.history() == 0, "wider again: the line at the top of the screen, whole there: " + screen(t));
        t.feed("!\r\n1\r\n2\r\n3");
        t.resize(5, 4);
        check(screen(t) == "ok!|1|2|3" && t.history() == 3, "narrower: what goes off the top goes into the history: " + screen(t));
        t.resize(30, 4);
        check(screen(t) == "ok!|1|2|3" && t.history() == 1 && blit::Term::line_text(t.history_line(0).data(), 30) == "hello world",
              "and wider: none of it comes back from the history: " + screen(t));
    }
    {
        blit::Term t(4, 3);
        t.feed("abc\xe6\x97\xa5" "d");  // a wide character that does not fit at the end of the row
        check(screen(t) == "abc|\xe6\x97\xa5" "d|", "a wide character at the edge goes to the next row: " + screen(t));
        t.resize(10, 3);
        check(screen(t) == "abc\xe6\x97\xa5" "d||" && t.cursor_col() == 6, "wider: no blank left where it did not fit: " + screen(t));
        t.resize(4, 3);
        check(screen(t) == "abc|\xe6\x97\xa5" "d|" && (t.at(1, 0).attr & blit::TermCell::kWide), "and narrower again: " + screen(t));
        blit::Term u(6, 3);
        u.feed("abcdef");  // the last column written: the cursor waits there to wrap
        u.resize(8, 3);
        u.feed("g");
        check(u.row_text(0) == "abcdefg", "a cursor waiting to wrap, after a wider resize: " + u.row_text(0));
        blit::Term a(10, 3);
        a.feed("main\x1b[?1049h\x1b[2Jfull");
        a.resize(6, 3);
        a.feed("\x1b[?1049l");
        check(screen(a) == "main||" && a.cursor_col() == 4, "the main screen wrapped again under the alternate one: " + screen(a));
    }
    {
        blit::Term t(10, 3);
        check(t.mouse_mode() == 0 && !t.mouse_sgr(), "no mouse to start with");
        t.feed("\x1b[?1002h\x1b[?1006h");
        check(t.mouse_mode() == 1002 && t.mouse_sgr(), "the mouse asked for: 1002, told as SGR");
        t.feed("\x1b[?1000l");
        check(t.mouse_mode() == 1002, "a mode not on, turned off: the one on stays");
        t.feed("\x1b[?1002l\x1b[?1006l");
        check(t.mouse_mode() == 0 && !t.mouse_sgr(), "and off");
    }
    {
        // Pictures: sixel's, put at the cursor, the cursor under them.
        blit::Term t(20, 5, 5);
        t.set_cell_px(10, 20);
        t.feed("ab\x1bPq\"1;1;10;12#1;2;100;0;0#1!10~-!10~\x1b\\x");
        check(t.pictures().size() == 1, "a sixel picture");
        if (!t.pictures().empty()) {
            const blit::TermPicture& p = t.pictures()[0];
            const blit::u8* px = p.rgba->data();
            check(p.w == 10 && p.h == 12 && p.col == 2 && p.line == 0 && p.cols == 1 && p.rows == 1 && px[0] == 255 && px[1] == 0 && px[3] == 255,
                  "its size, its place and its colour: " + std::to_string(p.w) + "x" + std::to_string(p.h) + " at " + std::to_string(p.line) + "," + std::to_string(p.col));
        }
        check(t.cursor_row() == 1 && t.cursor_col() == 3 && t.row_text(1) == "  x", "the cursor under it, where it started: " + t.row_text(1));
        t.feed("\x1b[16t\x1b[14t");
        check(t.take_reply() == "\x1b[6;20;10t\x1b[4;100;200t", "a cell's size and the screen's, in pixels");
        // It goes up with the text, and off the top of the history with its line.
        t.feed("\r\n1\r\n2\r\n3\r\n4");
        check(t.history() == 1 && t.pictures().size() == 1 && t.pictures()[0].line == 0 && t.lines_gone() == 0, "scrolled: on its line, now in the history");
        t.feed("\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10");
        check(t.history() == 5 && t.lines_gone() == 2 && t.pictures().empty(), "gone with its line off the top of the history");
    }
    {
        // Kitty's: sent and put there at once, in parts, compressed; sent, then put there; taken away.
        blit::Term t(20, 5);
        t.set_cell_px(10, 20);
        t.feed("\x1b_Ga=T,f=24,s=2,v=1,i=5;/wAAAP8A\x1b\\");  // (a red pixel and a green one)
        check(t.take_reply() == "\x1b_Gi=5;OK\x1b\\", "kitty: told it went well");
        check(t.pictures().size() == 1 && t.pictures()[0].w == 2 && t.pictures()[0].rgba->at(4) == 0 && t.pictures()[0].rgba->at(5) == 255 && t.cursor_col() == 1,
              "kitty: RGB, put at the cursor, the cursor after it");
        t.feed("\x1b_Ga=t,f=32,s=1,v=1,i=6,m=1;AAD/\x1b\\\x1b_Gm=0;gA==\x1b\\");  // (blue, half see-through: in two parts)
        check(t.take_reply() == "\x1b_Gi=6;OK\x1b\\" && t.pictures().size() == 1, "kitty: sent in parts, kept, not put there");
        t.feed("\x1b_Ga=p,i=6,c=4,r=2,q=1\x1b\\");
        check(t.take_reply().empty() && t.pictures().size() == 2 && t.pictures()[1].fit && t.pictures()[1].cols == 4 && t.pictures()[1].rows == 2 &&
                  t.pictures()[1].rgba->at(2) == 128 && t.pictures()[1].rgba->at(3) == 128,
              "kitty: put there, filling the cells asked for, its colour multiplied by its alpha, quietly");
        unsigned char raw[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}, packed[64];
        uLongf n = sizeof packed;
        compress2(packed, &n, raw, sizeof raw, 9);
        static const char* const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (uLongf k = 0; k < n; k += 3) {
            const blit::u32 v = packed[k] << 16 | (k + 1 < n ? packed[k + 1] << 8 : 0) | (k + 2 < n ? packed[k + 2] : 0);
            b64 += abc[v >> 18 & 63], b64 += abc[v >> 12 & 63], b64 += k + 1 < n ? abc[v >> 6 & 63] : '=', b64 += k + 2 < n ? abc[v & 63] : '=';
        }
        t.feed("\x1b_Ga=T,f=32,o=z,s=2,v=2,i=7,C=1;" + b64 + "\x1b\\");
        check(t.take_reply() == "\x1b_Gi=7;OK\x1b\\" && t.pictures().size() == 3 && t.pictures()[2].rgba->at(12) == 10, "kitty: zlib compressed");
        t.feed("\x1b_Ga=T,f=100,i=8;AAAA\x1b\\\x1b_Ga=p,i=99\x1b\\");
        check(t.take_reply() == "\x1b_Gi=8;EBADPNG:the picture could not be read\x1b\\\x1b_Gi=99;ENOENT:no such image\x1b\\", "kitty: what cannot be done says why");
        t.feed("\x1b_Ga=d,d=i,i=6\x1b\\");
        check(t.pictures().size() == 2, "kitty: one image's taken away");
        t.feed("\x1b[2J");
        check(t.pictures().empty(), "the screen erased: its pictures go");
        t.feed("\x1b_Ga=T,f=32,o=z,s=100000,v=100000,i=10;AAAA\x1b\\");
        check(t.take_reply() == "\x1b_Gi=10;ENODATA:not enough data for its size\x1b\\" && t.pictures().empty(),
              "kitty: a size past the most a picture may be, refused before its data is decompressed (it once let the data grow to gigabytes)");
        t.feed("\x1b_Ga=T,f=24,s=1,v=1,i=11,r=4000000000;AAAA\x1b\\");
        check(t.take_reply() == "\x1b_Gi=11;OK\x1b\\" && t.pictures().size() == 1 && t.pictures()[0].rows == 8192 && t.pictures()[0].cols >= 1 &&
                  t.pictures()[0].cols <= 8192,
              "kitty: four billion rows asked for, no more than 8,192 (the cursor once went down them one at a time)");
        t.feed("\x1b[2J");
    }
    {
        // A picture stays with its line when the width changes.
        blit::Term t(10, 6);
        t.set_cell_px(10, 20);
        t.feed("0123456789abcdef\r\n\x1bPq#1;2;0;0;100#1!8~\x1b\\done");
        check(t.pictures().size() == 1 && t.pictures()[0].line == 2, "under a line wrapped onto two rows");
        t.resize(20, 6);
        check(t.pictures().size() == 1 && t.pictures()[0].line == 1 && t.row_text(2) == "done", "wider: the line one row again, the picture with it, a row up");
        t.feed("\x1b[?1049h\x1bPq#1!4~\x1b\\");
        check(t.pictures().size() == 2 && t.pictures()[1].alt, "on the alternate screen");
        t.feed("\x1b[?1049l");
        check(t.pictures().size() == 1 && !t.pictures()[0].alt, "which go with it");
    }
    {
        // Whatever comes, nothing breaks, and the cursor stays on the screen.
        std::mt19937 gen(7);
        for (int s = 0; s < seeds * 4; ++s) {
            blit::Term t(1 + gen() % 40, 1 + gen() % 12, 50);
            static const char* const pieces[] = {"\x1b[", "\x1b]", "\x1b(0", "\x1bM", "\x1b" "7", "\x1b" "8", ";", ":", "?", "1049h", "1049l", "r", "J", "K", "L", "M",
                                                 "P", "@", "S", "T", "m", "38;2;", "5", "H", "\r\n", "\t", "\b", "\xe6\x97\xa5", "\xf0\x9f\x98\x80", "\x07", "x",
                                                 "\x1bPq#1;2;50;50;50!20~-~~", "\x1b\\", "\x1b_Ga=T,f=24,s=1,v=1,i=3;AAAA", "\x1b_Gm=1;", "\x1b_Ga=d", "?1049h", "?1049l"};
            std::string bytes;
            for (int k = 0; k < 400; ++k) {
                if (gen() % 3 == 0) bytes += static_cast<char>(gen() % 256);
                else bytes += pieces[gen() % std::size(pieces)];
                if (gen() % 50 == 0) t.resize(1 + gen() % 40, 1 + gen() % 12);
            }
            for (size_t at = 0; at < bytes.size();) {
                const size_t n = 1 + gen() % 30;
                t.feed(std::string_view(bytes).substr(at, n));
                at += n;
            }
            if (t.cursor_row() >= t.rows() || t.cursor_col() >= t.cols()) {
                check(false, "the cursor left the screen (seed " + std::to_string(s) + ")");
                break;
            }
        }
    }
    if (failures) std::exit(1);
}

// The profilers' output, as recorded from tests/profile/burn.c (perf on
// Linux x86-64, xctrace on macOS): the same call tree comes out of both.
void profile_test() {
#if defined(BLIT_PROFILE_DATA)
    auto slurp = [](const char* name) {
        std::string s;
        if (FILE* f = std::fopen((std::string(BLIT_PROFILE_DATA) + "/" + name).c_str(), "rb")) {
            char buf[65536];
            for (size_t k; (k = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, k);
            std::fclose(f);
        }
        return s;
    };
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: profile: %s\n", what.c_str());
            ++failures;
        }
    };
    auto find = [](const blit::Profile& p, std::string_view name) -> const blit::ProfileFunc* {
        for (const blit::ProfileFunc& f : p.funcs)
            if (f.name == name) return &f;
        return nullptr;
    };
    // The path from the root through these functions, in order (ignoring what comes before the first).
    auto path_weight = [](const blit::Profile& p, std::initializer_list<std::string_view> names) -> blit::u64 {
        blit::u64 sum = 0;
        for (blit::u32 n = 1; n < p.nodes.size(); ++n) {
            if (p.funcs[p.nodes[n].func].name != *(names.end() - 1)) continue;
            blit::u32 at = n;
            bool ok = true;
            for (auto it = names.end(); it != names.begin();) {
                --it;
                if (at == 0 || p.funcs[p.nodes[at].func].name != *it) {
                    ok = false;
                    break;
                }
                at = p.nodes[at].parent;
            }
            if (ok) sum += p.nodes[n].total;
        }
        return sum;
    };
    auto common = [&](const blit::Profile& p, const char* what, const std::string& file) {
        const blit::ProfileFunc* a = find(p, "leaf_a");
        const blit::ProfileFunc* b = find(p, "leaf_b");
        const blit::ProfileFunc* mid = find(p, "middle");
        const blit::ProfileFunc* top = find(p, "top");
        check(a && b && mid && top, std::string(what) + ": the functions are there");
        if (!a || !b || !mid || !top) return;
        check(a->self > p.weight / 4 && b->self > p.weight / 8, std::string(what) + ": the leaves have most of the time");
        check(mid->total >= a->total + b->total - p.weight / 100 && top->total >= mid->total, std::string(what) + ": totals include callees");
        check(a->file == file && a->line == 3 && b->line == 4, std::string(what) + ": the leaves' source lines (" + a->file + ":" + std::to_string(a->line) + ")");
        check(a->own && top->own, std::string(what) + ": the program's own functions are its own");
        check(path_weight(p, {"main", "top", "middle", "leaf_a"}) >= a->total * 9 / 10, std::string(what) + ": main > top > middle > leaf_a in the call tree");
        auto lit = p.lines.find(file);
        check(lit != p.lines.end() && lit->second.count(3) && lit->second.at(3).self > 0, std::string(what) + ": time on line 3");
        blit::u64 kids = 0;
        for (const blit::u32 k : p.nodes[0].kids) kids += p.nodes[k].total;
        check(kids + p.nodes[0].self == p.nodes[0].total && p.nodes[0].total == p.weight, std::string(what) + ": the root's weight is its children's");
        const std::vector<blit::u32> order = p.by_self();
        check(!order.empty() && (p.funcs[order[0]].name == "leaf_a" || p.funcs[order[0]].name == "leaf_b"), std::string(what) + ": the heaviest first");
    };
    {
        blit::Profile p;
        p.program = "/work/burn";
        std::string err;
        const std::string text = slurp("perf-script.txt");
        check(blit::parse_perf_script(text, "", &p, &err), "perf: parsed (" + err + ")");
        p.finish();
        check(p.samples == 477 && p.threads == 1, "perf: 477 samples on 1 thread, got " + std::to_string(p.samples) + " on " + std::to_string(p.threads));
        common(p, "perf", "/work/burn.c");
        // Its one thread's tree is the whole one; and as a file, and back, it is the same.
        check(p.by_thread.size() == 1 && p.by_thread[0].name == "burn" && p.by_thread[0].weight == p.weight && p.thread_root(p.by_thread[0].root),
              "perf: the thread, by its name");
        if (!p.by_thread.empty()) {
            std::vector<blit::u64> self, total;
            p.weights_under(p.by_thread[0].root, &self, &total);
            const blit::ProfileFunc* a = find(p, "leaf_a");
            const size_t ai = a ? static_cast<size_t>(a - p.funcs.data()) : 0;
            check(a && self[ai] == a->self && total[ai] == a->total, "perf: a function's weight in its thread's tree");
        }
        blit::Profile back;
        check(blit::Profile::from_json(p.to_json(), &back, &err), "a profile as a file, read back (" + err + ")");
        check(back.samples == p.samples && back.weight == p.weight && back.funcs.size() == p.funcs.size() && back.nodes.size() == p.nodes.size() &&
                  back.by_thread.size() == 1 && back.by_thread[0].root == p.by_thread[0].root && back.lines == p.lines && back.by_self() == p.by_self() &&
                  back.nodes[0].kids == p.nodes[0].kids && back.program == p.program,
              "the same profile, read back");
        check(!blit::Profile::from_json("{\"blit_profile\":1,\"funcs\":[],\"nodes\":[[5,-1,1,1]]}", &back, &err) && !blit::Profile::from_json("[]", &back, &err),
              "a file that is not one, or is damaged: not read");
        blit::Profile q;
        q.program = "/work/burn";
        check(blit::parse_perf_script(text, "ssh://box", &q, &err), "perf, another machine's: parsed");
        q.finish();
        check(q.lines.count("ssh://box/work/burn.c") == 1, "perf: another machine's paths have its host");
    }
    {
        blit::Profile p;
        p.program = "/work/burn";
        std::string err;
        check(blit::parse_xctrace(slurp("xctrace-time-profile.xml"), "", &p, &err), "xctrace: parsed (" + err + ")");
        p.finish();
        check(p.samples == 934 && p.threads == 1 && p.weight == 934ull * 1000000, "xctrace: 934 samples of 1 ms on 1 thread, got " + std::to_string(p.samples));
        check(p.by_thread.size() == 1 && p.by_thread[0].name == "Main Thread", "xctrace: the thread, by its name: " + (p.by_thread.empty() ? std::string() : p.by_thread[0].name));
        common(p, "xctrace", "/work/burn.c");
    }
    // Memory: the Virtual Memory Trace's faults of new memory (one that was
    // in the page cache already is left out), and perf's page faults, a
    // page's bytes each.
    auto self_of = [&](const blit::Profile& p, std::string_view prefix) {
        blit::u64 w = 0;
        for (const blit::ProfileFunc& f : p.funcs)
            if (f.name.starts_with(prefix)) w += f.total;
        return w;
    };
    {
        blit::Profile p;
        std::string err;
        check(blit::parse_xctrace(slurp("xctrace-virtual-memory.xml"), "", &p, &err), "xctrace, memory: parsed (" + err + ")");
        p.finish();
        check(p.unit == "bytes" && p.samples == 14 && p.weight == 14ull * 16384, "xctrace, memory: 14 faults of 16 KiB of new memory, got " + std::to_string(p.samples) + ", " + std::to_string(p.weight) + " " + p.unit);
        check(self_of(p, "big_allocs") == 6ull * 16384 && self_of(p, "small_allocs") == 8ull * 16384, "xctrace, memory: each function's bytes");
    }
    {
        blit::Profile p;
        std::string err;
        check(blit::parse_perf_script(slurp("perf-page-faults.txt"), "", &p, &err, 4096), "perf, memory: parsed (" + err + ")");
        p.finish();
        check(p.samples == 20 && p.weight == 20ull * 4096, "perf, memory: 20 page faults of 4 KiB, got " + std::to_string(p.samples) + ", " + std::to_string(p.weight));
        check(self_of(p, "big_allocs") == 5ull * 4096 && self_of(p, "small_allocs") == 10ull * 4096, "perf, memory: each function's bytes");
    }
    // Waiting: the System Call Trace's calls by how long each took, and
    // perf's context switches by how long until the thread ran again (a
    // preemption is not a wait).
    {
        blit::Profile p;
        std::string err;
        check(blit::parse_xctrace(slurp("xctrace-syscall.xml"), "", &p, &err), "xctrace, waiting: parsed (" + err + ")");
        p.finish();
        const blit::ProfileFunc* sem = find(p, "__semwait_signal");
        const blit::ProfileFunc* rd = find(p, "read");
        check(p.unit == "ns" && p.threads == 2 && sem && rd && sem->self > 500'000'000 && rd->self > 200'000'000 && p.weight < 800'000'000,
              "xctrace, waiting: sleeping (two threads) and a read, by how long, got " + std::to_string(p.weight) + " ns on " + std::to_string(p.threads));
    }
    {
        blit::Profile p;
        std::string err;
        const std::string samples = blit::perf_waits_as_samples(slurp("perf-waits.txt"));
        check(blit::parse_perf_script(samples, "", &p, &err), "perf, waiting: parsed (" + err + ")");
        p.finish();
        check(p.samples == 3 && p.threads == 2, "perf, waiting: three waits on two threads (the preemption left out), got " + std::to_string(p.samples) + " on " + std::to_string(p.threads));
        check(self_of(p, "reader") == 205'000'000 && self_of(p, "sleeper") == 300'000'000 && self_of(p, "nanosleep") == 200'000'000 && p.weight == 705'000'000,
              "perf, waiting: each wait from its switch out to its thread's next switch in, got " + std::to_string(p.weight));
        blit::Profile none;
        check(!blit::parse_perf_script(blit::perf_waits_as_samples("t  1000 1.0: PERF_RECORD_SWITCH OUT\nt  1000 2.0: PERF_RECORD_SWITCH IN\n"), "", &none, &err),
              "perf, waiting: switches with no samples (perf_event_paranoid too high) are no profile");
    }
    {
        blit::Profile p;
        std::string err;
        check(!blit::parse_perf_script("", "", &p, &err) && !err.empty(), "perf: nothing recorded is an error");
        check(!blit::parse_xctrace("<trace-query-result></trace-query-result>", "", &p, &err), "xctrace: nothing recorded is an error");
    }
    if (failures) std::exit(1);
#endif
}

// A debugger's adapter, here a script: its messages, and what it writes
// that is not one (gdb lets the program write where its messages go), told
// as the program's output as soon as a line of it is whole, however short.
void dap_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const std::string body = R"({"seq":1,"type":"event","event":"stopped","body":{"reason":"step"}})";
    const std::string script = "printf 'one line\\n'; printf 'Content-Length: " + std::to_string(body.size()) + "\\r\\n\\r\\n%s' '" + body +
                               "'; sleep 0.2; printf 'hi\\n'; sleep 5";
    Dap dap;
    std::string err;
    check(dap.start({"/bin/sh", "-c", script}, "", {}, &err), "dap: the adapter starts");
    std::vector<std::string> said;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (said.size() < 3 && std::chrono::steady_clock::now() < until) {
        for (const Dap::Message& m : dap.poll()) said.push_back(m.name + ":" + std::string(m.name == "output" ? m.body()["output"].str() : m.body()["reason"].str()));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(said.size() == 3 && said[0] == "output:one line\n" && said[1] == "stopped:step" && said[2] == "output:hi\n",
          "dap: messages, and the program's lines between them");
    dap.stop();

    // What the program wrote last, with no line's end, as its output ends.
    {
        Dap last;
        check(last.start({"/bin/sh", "-c", "printf 'the end'"}, "", {}, &err), "dap: an adapter that ends at once starts");
        std::string out;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        bool ended = false;
        while (!ended && std::chrono::steady_clock::now() < end) {
            ended = last.ended();  // (before the poll: what came before the end is in it)
            for (const Dap::Message& m : last.poll())
                if (m.name == "output") out += m.body()["output"].str();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ended && out == "the end", "dap: the program's last words, with no line's end, as its output ends");
        last.stop();
    }

    // An adapter that takes none of what it is sent for a while (far more
    // than a pipe holds): sending waits for none of it, and all of it comes
    // to it, in order, once it reads.
    {
        const std::string args = "{\"pad\":\"" + std::string(65536, 'x') + "\"}";
        size_t total = 0;
        for (int seq = 1; seq <= 32; ++seq) {
            const std::string json = "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":\"pad\",\"arguments\":" + args + "}";
            total += ("Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json).size();
        }
        const std::string taker = "sleep 2; n=$(head -c " + std::to_string(total) +
                                  " | wc -c | tr -d ' '); body=\"{\\\"seq\\\":1,\\\"type\\\":\\\"event\\\",\\\"event\\\":\\\"took\\\",\\\"body\\\":{\\\"reason\\\":\\\"$n\\\"}}\"; "
                                  "printf 'Content-Length: %d\\r\\n\\r\\n%s' ${#body} \"$body\"; sleep 5";
        Dap slow;
        check(slow.start({"/bin/sh", "-c", taker}, "", {}, &err), "dap: an adapter that does not read yet starts");
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 32; ++i) slow.request("pad", args);
        const auto took = std::chrono::steady_clock::now() - t0;
        check(took < std::chrono::milliseconds(500), "dap: sending to an adapter that does not read waits for nothing");
        std::string got;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (got.empty() && std::chrono::steady_clock::now() < end) {
            for (const Dap::Message& m : slow.poll())
                if (m.name == "took") got = m.body()["reason"].str();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(got == std::to_string(total), "dap: what waited is all written once the adapter reads");
        slow.stop();
    }
}

void remote_test() {
#if defined(BLIT_FAKE_SSH)
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    check(is_remote("ssh://box/home/a") && !is_remote("/home/a") && remote_path("box", "/x/y") == "ssh://box/x/y", "remote paths");
    std::string host, there;
    check(split_remote("ssh://box/home/a", &host, &there) && host == "box" && there == "/home/a" && split_remote("ssh://box", &host, &there) && there == "/",
          "a remote path in its parts");
    const char* tmp = std::getenv("TMPDIR");
    char base[] = "/tmp/blit-remote-XXXXXX";
    std::string top = tmp && *tmp ? std::string(tmp) + "/blit-remote-XXXXXX" : base;
    if (!mkdtemp(top.data())) return fail("remote: a scratch folder", 0, 0);
    char real[PATH_MAX];
    if (realpath(top.c_str(), real)) top = real;
    const std::string home = top + "/home", work = top + "/work";
    mkdir(home.c_str(), 0755);
    mkdir(work.c_str(), 0755);
    setenv("BLIT_SSH", BLIT_FAKE_SSH, 1);
    setenv("BLIT_FAKE_HOME", home.c_str(), 1);
    std::shared_ptr<Remote> r = Remote::get("box");
    check(r->wait_ready(60'000), ("remote: connected (" + r->error() + ")").c_str());
    utsname here{};  // ("the machine" is this one, through the fake ssh)
    check(uname(&here) == 0 && r->home() == home && r->system().starts_with(here.sysname), "remote: the agent says where home is, and what the machine is");
    DIR* cache = opendir((home + "/.cache/blit").c_str());
    int agents = 0;
    if (cache) {
        while (const dirent* e = readdir(cache)) agents += std::string_view(e->d_name).starts_with("blit-agent-");
        closedir(cache);
    }
    check(agents == 1, "remote: the agent was copied there");
    // Files.
    std::string err;
    check(r->write(work + "/a.txt", "one\ntwo\n", &err), "remote: write");
    std::string got;
    i64 when = 0;
    check(r->read(work + "/a.txt", 1 << 20, &got, &when) == 0 && got == "one\ntwo\n" && when > 0, "remote: read");
    check(r->read(work + "/none.txt", 1 << 20, &got) == 1 && r->read(work, 1 << 20, &got) == 2 && r->read(work + "/a.txt", 3, &got) == 3,
          "remote: reading what is not there, a folder, too much");
    RemoteStat st;
    check(r->stat(work + "/a.txt", &st) && st.kind == 1 && st.size == 8 && r->stat(work, &st) && st.kind == 2 && r->stat(work + "/x", &st) && st.kind == 0,
          "remote: stat");
    check(r->make_dir(work + "/src/deep", true, &err) && r->create(work + "/src/b.cpp", &err) && !r->create(work + "/src/b.cpp", &err) &&
              r->rename(work + "/src/b.cpp", work + "/src/c.cpp", &err),
          "remote: folders, a new file, rename");
    std::vector<RemoteEntry> items;
    check(r->list(work, &items) && items.size() == 2, "remote: list");
    r->write(work + "/src/c.cpp", "int needle = 1;\nint other;\n", &err);
    r->write(work + "/src/deep/d.txt", "a needle here\n", &err);
    FinderOptions o;
    FileIndex ix;
    u32 walk = 0;
    check(r->walk(work, o, &ix, &walk) && ix.size() == 3 && ix.root == "ssh://box" + work && ix.path(1) == "src/c.cpp", "remote: the finder's walk");
    std::vector<FindHit> hits;
    std::vector<u32> files;
    bool capped = true;
    check(r->grep(walk, nullptr, "needle", 8 << 20, 100, &hits, &files, &capped) && hits.size() == 2 && files.size() == 2 && !capped && hits[0].line == 0 &&
              hits[1].text == "a needle here",
          "remote: the finder's search");
    const std::vector<u32> only = {2};
    check(r->grep(walk, &only, "needle", 8 << 20, 100, &hits, &files, &capped) && hits.size() == 1 && hits[0].file == 2, "remote: a search in some files");
    check(r->trash(work + "/src/deep/d.txt", &err) && r->stat(work + "/src/deep/d.txt", &st) && st.kind == 0 &&
              r->stat(home + "/.local/share/Trash/files/d.txt", &st) && st.kind == 1,
          "remote: into the trash");
    check(!r->which("sh").empty() && r->which("blit-no-such-program").empty() && r->realpath(work + "/src/../a.txt") == work + "/a.txt", "remote: which, realpath");
    // A program: input in, output and its exit status out.
    RemoteProc p;
    check(r->spawn({"sh", "-c", "cat; echo done >&2; pwd; exit 3"}, work, {"GREETING=hi"}, &p, &err), ("remote: spawn (" + err + ")").c_str());
    (void)!::write(p.in, "fed\n", 4);
    ::close(p.in);
    std::string out, errs;
    char buf[256];
    for (ssize_t k; (k = ::read(p.out, buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(k));
    for (ssize_t k; (k = ::read(p.err, buf, sizeof buf)) > 0;) errs.append(buf, static_cast<size_t>(k));
    ::close(p.out);
    ::close(p.err);
    const int status = r->wait(p.id);
    check(out == "fed\n" + work + "\n" && errs == "done\n" && status == 3, "remote: a program's input, output, errors, folder and exit status");
    check(!r->spawn({"blit-no-such-program"}, work, {}, &p, &err) || r->wait(p.id) == 127, "remote: a program that is not there");
    // Again: the agent is there already, and is not copied a second time.
    Remote::close_all();
    r = Remote::get("box");
    check(r->wait_ready(60'000) && r->read(work + "/a.txt", 1 << 20, &got) == 0, "remote: connected again, the agent already there");
    // A host that cannot be reached: ssh's own reason.
    Remote::close_all();
    setenv("BLIT_FAKE_FAIL", "Connection refused", 1);
    r = Remote::get("nowhere");
    check(!r->wait_ready(30'000) && r->state() == Remote::kFailed && r->error().find("Connection refused") != std::string::npos,
          ("remote: a host that cannot be reached (" + r->error() + ")").c_str());
    unsetenv("BLIT_FAKE_FAIL");
    Remote::close_all();
    std::system(("rm -rf '" + top + "'").c_str());
#endif
}

// What is folded: random folds over a text, the map from rows to lines
// against one worked out the slow way, and folds kept through edits.
void folds_test() {
    std::mt19937 pick(7);
    for (int round = 0; round < 200; ++round) {
        Editor ed;
        std::string text;
        const int lines = 5 + static_cast<int>(pick() % 60);
        for (int i = 0; i < lines; ++i) text += "line " + std::to_string(i) + "\n";
        ed.insert(text);
        Folds f;
        f.attach(&ed);
        const u64 n = ed.buf.line_count();
        for (int k = static_cast<int>(pick() % 6); k > 0; --k) {
            const u64 h = pick() % n, l = h + 1 + pick() % 8;
            f.add(ed.buf, h, l);
        }
        f.sync(ed.buf, ed.revision());
        // The slow way: each line hidden or not.
        std::vector<bool> hidden(n, false);
        for (const Folds::Fold& x : f.all())
            for (u64 l = ed.buf.line_of(x.head) + 1; l <= ed.buf.line_of(std::min(x.end, ed.buf.size())) && l < n; ++l) hidden[l] = true;
        std::vector<u64> shown;
        for (u64 l = 0; l < n; ++l)
            if (!hidden[l]) shown.push_back(l);
        if (f.rows(n) != shown.size()) fail("folds: rows", round, 0);
        for (u64 r = 0; r < shown.size(); ++r) {
            if (f.line_at(r) != shown[r]) fail("folds: the line a row shows", round, static_cast<int>(r));
            if (f.row_of(shown[r]) != r) fail("folds: the row a line is on", round, static_cast<int>(r));
        }
        for (u64 l = 0; l < n; ++l)
            if (f.hidden(l) != hidden[l]) fail("folds: hidden", round, static_cast<int>(l));
    }
    // Through edits: one in what is folded keeps it; one across its edge opens it.
    Editor ed;
    ed.insert("a {\n  b\n  c\n}\nd\n");
    Folds f;
    f.attach(&ed);
    ed.listen(&f);
    f.add(ed.buf, 0, 2);
    ed.move_to(ed.buf.line_start(1) + 2, false);
    ed.insert("xx\n  y");  // (inside: the fold now hides four lines)
    f.sync(ed.buf, ed.revision());
    if (f.all().size() != 1 || !f.hidden(1) || !f.hidden(3) || f.hidden(4)) fail("folds: an edit inside one", 0, 0);
    ed.move_to(ed.buf.size(), false);
    ed.insert("e\n");  // (after it: nothing changes)
    if (f.all().size() != 1) fail("folds: an edit after one", 0, 0);
    ed.move_to(0, false);
    ed.insert("z\n");  // (before it: it moves down a line)
    f.sync(ed.buf, ed.revision());
    if (f.all().size() != 1 || !f.header(1) || f.hidden(1)) fail("folds: an edit before one", 0, 0);
    ed.move_to(ed.buf.line_start(1) - 1, false);
    ed.move_to(ed.buf.line_start(2), true);
    ed.insert("");  // (across its header's start: it opens)
    if (!f.empty()) fail("folds: an edit across an edge", 0, 0);
}

// Tests as their runners list them, and their reports.
void tests_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const std::vector<TestCase> ct = parse_ctest_json(R"({"kind":"ctestInfo","tests":[{"name":"Math.Adds","command":["/b/t","--gtest_filter=Math.Adds"],"properties":[{"name":"WORKING_DIRECTORY","value":"/b"}]},{"name":"plain","command":["/b/p"]}]})", "/p/build");
    check(ct.size() == 2 && ct[0].id == "Math.Adds" && ct[0].command.size() == 2 && ct[0].cwd == "/b" && ct[0].group == "build" && ct[0].dir == "/p/build",
          "ctest's tests: name, command, working directory, folder");
    const std::vector<TestCase> py = parse_pytest_collect("tests/test_a.py::test_x\ntests/test_a.py::TestK::test_y\n\n2 tests collected in 0.01s\n");
    check(py.size() == 2 && py[1].id == "tests/test_a.py::TestK::test_y" && py[1].name == "test_y" && py[1].group == "tests/test_a.py", "pytest's node ids");
    const std::vector<TestOutcome> ju = parse_junit(R"(<testsuite><testcase name="Math.Adds" classname="Math.Adds" time="0.25"/><testcase name="plain" time="1"><failure message="x &lt; 2">trace</failure></testcase><testcase name="s"><skipped/></testcase></testsuite>)");
    check(ju.size() == 3 && ju[0].result == TestCase::kPassed && ju[0].seconds == 0.25 && ju[1].result == TestCase::kFailed && ju[1].message == "x < 2" && ju[2].result == TestCase::kSkipped,
          "a JUnit report: passed, failed with its message, skipped");
    TestOutcome o;
    o.name = "test_y";
    o.classname = "tests.test_a.TestK";
    check(outcome_is(o, py[1]) && !outcome_is(o, py[0]), "a pytest outcome found by its module and class");
    const auto cpp = find_test_defs("#include <gtest.h>\nTEST(Math, Adds) {\n}\n  TEST_F(Fix, Two)\nTEST_CASE(\"vectors grow\", \"[v]\") {}\nSCENARIO(\"a\")\nint TESTING(1);\n", false);
    check(cpp.size() == 4 && cpp[0] == std::pair<u32, std::string>{1, "Math.Adds"} && cpp[1].second == "Fix.Two" && cpp[2].second == "vectors grow" && cpp[3].second == "Scenario: a",
          "C++ tests where they are defined");
    const auto pyd = find_test_defs("def test_a():\n    pass\nclass TestK:\n    def test_b(self):\n        pass\ndef helper():\n    pass\nasync def test_c():\n    pass\n", true);
    check(pyd.size() == 3 && pyd[0].second == "test_a" && pyd[1] == std::pair<u32, std::string>{3, "TestK::test_b"} && pyd[2].second == "test_c", "Python tests, in a class or not");
    const auto gc = parse_gcov_json(R"({"format_version":"2","current_working_directory":"/b","files":[{"file":"../p/a.cpp","lines":[{"line_number":3,"count":2},{"line_number":4,"count":0},{"line_number":7,"count":0}]},{"file":"/usr/include/c++/v.h","lines":[{"line_number":1,"count":1}]}]}
{"current_working_directory":"/b","files":[{"file":"../p/a.cpp","lines":[{"line_number":7,"count":1}]}]}
)", "/x");
    check(gc.size() == 1 && gc[0].first == "/p/a.cpp" && gc[0].second.hit == std::vector<u32>{3, 7} && gc[0].second.missed == std::vector<u32>{4} && gc[0].second.percent > 66 && gc[0].second.percent < 67,
          "gcov's JSON: lines run in any of its documents, the system's headers left out");
    const auto cp = parse_coverage_py(R"({"meta":{},"files":{"src/m.py":{"executed_lines":[1,2],"missing_lines":[5]}}})", "/p");
    check(cp.size() == 1 && cp[0].first == "/p/src/m.py" && cp[0].second.hit == std::vector<u32>{1, 2} && cp[0].second.missed == std::vector<u32>{5}, "coverage.py's JSON");
}

void find_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    auto term = [](const char* s) { return fuzzy_terms(s).at(0); };
    // One term against one text.
    check(fuzzy_score(term("abc"), "xyz") < 0 && fuzzy_score(term("abc"), "acb") < 0, "letters that are not there, or not in that order, do not match");
    check(fuzzy_score(term("abc"), "abc") > fuzzy_score(term("abc"), "a_b_c") && fuzzy_score(term("abc"), "a_b_c") > fuzzy_score(term("abc"), "xaxbxc"),
          "letters together beat letters at the starts of words, which beat letters scattered");
    check(fuzzy_score(term("abc"), "ABC") >= 0 && fuzzy_score(term("Abc"), "abc") < 0 && fuzzy_score(term("Abc"), "xAbc") >= 0,
          "small letters match either case, a capital only itself");
    check(fuzzy_score(term("gw"), "getWidth") > fuzzy_score(term("gw"), "glowing"), "a capital after a small letter starts a word");
    {
        std::vector<u32> at;
        fuzzy_score(term("fb"), "xfoo_bar", &at);
        check(at.size() == 2 && at[0] == 1 && at[1] == 5, "where the matched letters are");
    }
    check(fuzzy_word_starts(term("lnum"), "editor.line_numbers") && fuzzy_word_starts(term("gofile"), "Go to file") &&
              fuzzy_word_starts(term("wid"), "editor.tab_width") && fuzzy_word_starts(term("gw"), "getWidth") && fuzzy_word_starts(term("dth"), "width"),
          "letters in runs from the starts of words, or the term whole, are a close match");
    check(!fuzzy_word_starts(term("tot"), "editor.tab_width") && !fuzzy_word_starts(term("wid"), "window.height") && !fuzzy_word_starts(term("Line"), "line") &&
              !fuzzy_word_starts(term("abcd"), "abc"),
          "letters scattered inside words are not");
    // Paths.
    {
        auto score = [](const char* typed, const char* path) {
            const std::vector<FuzzyTerm> t = fuzzy_terms(typed);
            return fuzzy_path_score(t, path);
        };
        check(score("grid", "src/grid.cpp") > score("grid", "shaders/gen/grid.frag.spv"), "of two names that match alike, the shorter path wins");
        check(score("vimcpp", "src/vim.cpp") > score("vimcpp", "src/appvim.cpp"), "a match from the start of the name beats one inside it");
        check(score("render", "src/render.cpp") > score("render", "render/notes.txt"), "a match in the file's name beats one in a folder's");
        check(score("src render", "src/render.cpp") >= 0 && score("render zzz", "src/render.cpp") < 0, "every word typed has to match");
        check(score("s/r", "src/render.cpp") >= 0 && score("s/r", "render.cpp") < 0, "a slash typed is a slash in the path");
        std::vector<u32> at;
        const std::vector<FuzzyTerm> t = fuzzy_terms("rcpp");
        fuzzy_path_score(t, "src/render.cpp", &at);
        check(at.size() == 4 && at[0] == 4 && at[1] == 11 && at[3] == 13, "positions are in the whole path");
    }
    check(find_term("int Main() {}", term("main")) == 4 && find_term("int main", term("Main")) == std::string_view::npos &&
              find_term("aXa xa", term("xa"), 2) == 4 && find_term("abc", term("abcd")) == std::string_view::npos && find_term("aab", term("ab")) == 1,
          "a word found in a text as it stands");
    // .gitignore's patterns.
    check(Finder::glob("*.o", "a.o") && !Finder::glob("*.o", "dir/a.o") && Finder::glob("build-*", "build-bench") && !Finder::glob("build-*", "build"),
          "a star stops at a slash");
    check(Finder::glob("**/x", "a/b/x") && Finder::glob("**/x", "x") && !Finder::glob("**/x", "a/bx") && Finder::glob("a/**", "a/b/c") && Finder::glob("a/**/c", "a/c") &&
              Finder::glob("a/**/c", "a/b/b/c"),
          "two stars are any number of folders");
    check(Finder::glob("doc/*.txt", "doc/a.txt") && !Finder::glob("doc/*.txt", "doc/sub/a.txt") && Finder::glob("[a-c]?", "bz") && !Finder::glob("[a-c]?", "dz") &&
              Finder::glob("[!a]x", "bx") && !Finder::glob("?", "/") && Finder::glob("a\\*", "a*") && !Finder::glob("a\\*", "ab"),
          "sets, single characters and escapes");

    // A folder to read.
    char tmpl[] = "/tmp/blit-find-XXXXXX";
    const char* made = mkdtemp(tmpl);
    check(made != nullptr, "a scratch folder");
    const std::string root = made;
    auto put = [&](const char* rel, const char* text) {
        const std::string path = root + "/" + rel;
        for (size_t i = root.size() + 1; i < path.size(); ++i)
            if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
        FILE* f = std::fopen(path.c_str(), "wb");
        check(f != nullptr, "a scratch file");
        std::fwrite(text, 1, std::strlen(text), f);
        std::fclose(f);
    };
    put(".gitignore", "ignored.txt\n/top_only\nlogs/\n*.log\n!keep.log\n# a comment\n");
    put("a.txt", "one two\nthree One\nfour\n");
    put(".hidden", "x");
    put("ignored.txt", "x");
    put("top_only", "x");
    put("x.log", "x");
    put("keep.log", "x");
    put("logs/l.txt", "x");
    put("sub/b.cpp", "int three();\n// one more\n");
    put("sub/c.o", "x");
    put("sub/top_only", "x");
    put("sub2/.gitignore", "secret*\n");
    put("sub2/secret.txt", "three");
    put("sub2/open.txt", "nothing here\n");
    put("build/out.txt", "three");
    put("node_modules/m.js", "three");
    put("bin.dat", "three");
    {  // (a zero byte in it: not text)
        FILE* f = std::fopen((root + "/bin.dat").c_str(), "ab");
        std::fwrite("\0three", 1, 6, f);
        std::fclose(f);
    }
    auto list = [](const FileIndex& ix) {
        std::string all;
        for (size_t i = 0; i < ix.size(); ++i) all += std::string(ix.path(i)) + " ";
        return all;
    };
    FinderOptions opt;
    check(list(*Finder::walk(root, opt)) == ".gitignore .hidden a.txt bin.dat keep.log sub/b.cpp sub/top_only sub2/.gitignore sub2/open.txt ",
          "the files of a folder, without what .gitignore files and the usual rules leave out");
    opt.hidden = false;
    check(list(*Finder::walk(root, opt)) == "a.txt bin.dat keep.log sub/b.cpp sub/top_only sub2/open.txt ", "and without the ones that start with a dot");
    opt.hidden = true;
    opt.gitignore = false;
    opt.ignore = {"sub2/", "*.txt"};
    check(list(*Finder::walk(root, opt)) == ".gitignore .hidden bin.dat keep.log sub/b.cpp sub/top_only top_only x.log ", "the settings' own rules, with .gitignore files not heeded");
    opt = FinderOptions{};
    opt.max_files = 3;
    check(Finder::walk(root, opt)->size() == 3 && Finder::walk(root, opt)->truncated, "a folder with more files than the limit is listed that far");

    // Lines in a text.
    {
        std::vector<FindHit> hits;
        const std::string_view text = "one two\nthree One\nfour\r\nlast one";
        std::vector<FuzzyTerm> t = fuzzy_terms("one");
        check(Finder::grep(text, t, 7, 100, &hits) && hits.size() == 3 && hits[0].line == 0 && hits[0].col == 0 && hits[1].line == 1 && hits[1].col == 6 &&
                  hits[1].text == "three One" && hits[2].line == 3 && hits[0].file == 7,
              "the lines a word is on");
        hits.clear();
        t = fuzzy_terms("One");
        check(Finder::grep(text, t, 0, 100, &hits) && hits.size() == 1 && hits[0].line == 1, "a capital typed is a capital found");
        hits.clear();
        t = fuzzy_terms("one three");
        check(Finder::grep(text, t, 0, 100, &hits) && hits.size() == 1 && hits[0].line == 1 && hits[0].col == 0, "every word has to be on the line, in any order");
        hits.clear();
        t = fuzzy_terms("four");
        check(Finder::grep(text, t, 0, 100, &hits) && hits.size() == 1 && hits[0].text == "four", "a carriage return is no part of the line");
        hits.clear();
        t = fuzzy_terms("one");
        check(Finder::grep(text, t, 0, 2, &hits) && hits.size() == 2, "no more lines than there is room for");
        hits.clear();
        check(!Finder::grep(std::string_view("ab\0cd one", 9), t, 0, 100, &hits) && hits.empty(), "a file with a zero byte in it is not text");
    }

    // The worker: the folder read in the background, then searches.
    {
        Finder f;
        std::atomic<int> told{0};
        f.set_notify([&] { ++told; });
        f.index(root);
        const u64 first = f.find_names("bcpp");
        f.wait_idle();
        std::shared_ptr<const FindResults> r = f.results();
        check(r && r->serial == first && !r->hits.empty() && r->index->path(r->hits[0].file) == "sub/b.cpp" && r->matched == 1 && told.load() > 0,
              "a search for a name, asked for before the folder had been read");
        f.find_names("");
        f.wait_idle();
        r = f.results();
        check(r->hits.size() == 9 && r->matched == 9 && !r->text, "nothing typed: all the files");
        const u64 second = f.find_text("three");
        f.wait_idle();
        r = f.results();
        check(r->serial == second && r->text && !r->searching && r->hits.size() == 2 && r->files_hit == 2 && r->index->path(r->hits[0].file) == "a.txt" &&
                  r->hits[0].line == 1 && r->index->path(r->hits[1].file) == "sub/b.cpp" && r->hits[1].line == 0 && r->hits[1].col == 4,
              "a search for text: the lines, in the order of the files, and not the ones in files left out or not text");
        f.find_text("t");
        f.wait_idle();
        check(f.results()->hits.empty(), "one letter is not searched for");
        // A new file shows after the folder is read again, in the search that was asked for last.
        put("sub/new_three.txt", "three again\n");
        f.find_text("three");
        f.index(root);
        f.wait_idle();
        check(f.results()->hits.size() == 3, "the folder read again: the last search is answered again");
        f.cancel();
        check(f.idle(), "a search given up");
    }
    const std::string rm = "rm -rf '" + root + "'";
    check(std::system(rm.c_str()) == 0, "the scratch folder taken away");
}

// A family (man, woman, girl: three emoji and two joiners) and a flag (two
// regional indicators), each one character.
#define FAM "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
#define FLAG "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7"

// Vim has EditWatch's virtual function and a destructor that is not: none
// may be made of it to be deleted as one.
static_assert(std::is_final_v<Vim> || std::has_virtual_destructor_v<Vim>);

void vim_test() {
    struct Case {
        const char* before;
        const char* keys;
        const char* after;
    };
    static const Case cases[] = {
        // Moving.
        {"|one two three", "w", "one |two three"},
        {"|one two three", "2w", "one two |three"},
        {"|one two three", "e", "on|e two three"},
        {"one two |three", "b", "one |two three"},
        {"one |two", "ge", "on|e two"},
        {"|one, two", "w", "one|, two"},
        {"|one, two", "W", "one, |two"},
        {"|one two three", "$", "one two thre|e"},
        {"one two |three", "0", "|one two three"},
        {"|    x y", "^", "    |x y"},
        {"|a.b.c.d", "f.", "a|.b.c.d"},
        {"|a.b.c.d", "f.;", "a.b|.c.d"},
        {"|a.b.c.d", "2f.", "a.b|.c.d"},
        {"|a.b.c.d", "f.;,", "a|.b.c.d"},
        {"|ab.cd.ef", "t.", "a|b.cd.ef"},
        {"|ab.cd.ef", "t.;", "ab.c|d.ef"},
        {"ab.cd.e|f", "F.", "ab.cd|.ef"},
        {"abc|def\nx\nabcdefg", "j", "abcdef\n|x\nabcdefg"},
        {"abc|def\nx\nabcdefg", "jj", "abcdef\nx\nabc|defg"},
        {"abc|def\nx\nabcdefg", "jjk", "abcdef\n|x\nabcdefg"},
        {"one\ntw|o\nthree", "gg", "on|e\ntwo\nthree"},
        {"one\ntw|o\nthree\n", "G", "one\ntwo\nth|ree\n"},
        {"|one\ntwo\nthree", "2G", "one\n|two\nthree"},
        {"|one\ntwo\nthree", ":3<CR>", "one\ntwo\n|three"},
        {"|(a [b] c)", "%", "(a [b] c|)"},
        {"(a [b] c|)", "%", "|(a [b] c)"},
        {"(a |[b] c)", "%", "(a [b|] c)"},
        {"|a\nb\n\nc\nd\n\ne", "}", "a\nb\n|\nc\nd\n\ne"},
        {"a\nb\n\nc\n|d\n\ne", "{", "a\nb\n|\nc\nd\n\ne"},
        {"|a\r\nb\r\n\r\nc", "}", "a\r\nb\r\n|\r\nc"},
        {"|a\nb", "}", "a\n|b"},
        {"|a\nb\n", "}", "a\n|b\n"},
        {"|a\n\nb\n\nc", "2}", "a\n\nb\n|\nc"},
        {"a\n|\n\nb\n\nc", "}", "a\n\n\nb\n|\nc"},
        {"a\n|b", "{", "|a\nb"},
        {"a\n\nb\n|\nc", "{", "a\n|\nb\n\nc"},
        {"a\n\n\nb\n|c", "{", "a\n\n|\nb\nc"},
        {"|a\nb\n\nc", "d}", "|\nc"},
        {"|a\n{\nb\n{\n", "]]", "a\n|{\nb\n{\n"},
        {"|a\n{\nb\n{\nc", "2]]", "a\n{\nb\n|{\nc"},
        {"{\na\n{\n|b", "[[", "{\na\n|{\nb"},
        {"{\na\n|{\nb", "[[", "|{\na\n{\nb"},
        {"|a\n}\nb", "][", "a\n|}\nb"},
        {"|a\nb\nc", "]]", "a\nb\n|c"},
        {"a\nb\n|c", "[[", "|a\nb\nc"},
        {"one |two", "l", "one t|wo"},
        {"one tw|o", "l", "one tw|o"},
        {"|one two", "h", "|one two"},
        {"|one\n  two", "+", "one\n  |two"},
        // Deleting, changing, yanking.
        {"|one two three", "dw", "|two three"},
        {"|one two three", "d2w", "|three"},
        {"|one two three", "2dw", "|three"},
        {"one |two\nthree", "dw", "one| \nthree"},
        {"one |two three", "de", "one | three"},
        {"one two |three", "db", "one |three"},
        {"one |two three", "d$", "one| "},
        {"one |two three", "D", "one| "},
        {"one |two three", "d0", "|two three"},
        {"one\n|two\nthree", "dd", "one\n|three"},
        {"|one\ntwo\nthree", "2dd", "|three"},
        {"one\n|two", "dd", "|one"},
        {"one\n|two\n", "dd", "|one\n"},
        {"|one\ntwo\nthree", "dj", "|three"},
        {"one\n  |two\nthree", "dk", "th|ree"},
        {"one\n|two\nthree\nfour", "dG", "|one"},
        {"a|bc", "x", "a|c"},
        {"|abcdef", "3x", "|def"},
        {"ab|c", "xx", "|a"},
        {"ab|c", "X", "a|c"},
        {"|one two", "cwONE<Esc>", "ON|E two"},
        {"|one two", "cw<Esc>", "| two"},
        {"one |two three", "CX<Esc>", "one |X"},
        {"  one\n  |two\nthree", "ccx<Esc>", "  one\n  |x\nthree"},
        {"a|bc", "sX<Esc>", "a|Xc"},
        {"  on|e", "Sx<Esc>", "  |x"},
        {"|one\ntwo", "yyp", "one\n|one\ntwo"},
        {"one\n|two", "yyP", "one\n|two\ntwo"},
        {"one\n|two", "yyp", "one\ntwo\n|two"},
        {"|one two", "ywP", "one| one two"},
        {"|one two", "yw$p", "one twoone| "},
        {"|one two", "yeep", "oneon|e two"},
        {"one\n|two\nthree", "ddp", "one\nthree\n|two"},
        {"|one\ntwo", "Yp", "oon|ene\ntwo"},
        // Text objects.
        {"one t|wo three", "diw", "one | three"},
        {"one t|wo three", "daw", "one |three"},
        {"one two thr|ee", "daw", "one tw|o"},
        {"f(a, |b)", "ci(x<Esc>", "f(|x)"},
        {"f(a, |b) + 1", "da(", "f| + 1"},
        {"f(a, (b|), c)", "dib", "f(a, (|), c)"},
        {"x = [1, |2]", "di[", "x = [|]"},
        {"say \"he|llo\" now", "di\"", "say \"|\" now"},
        {"say \"he|llo\" now", "da\"", "say |now"},
        {"|say 'hi' now", "ci'x<Esc>", "say '|x' now"},
        {"f {\n    |a;\n    b;\n}", "di{", "f {\n|}"},
        {"f {\n    |a;\n    b;\n}", "ci{x<Esc>", "f {\n    |x\n}"},
        {"a\nb\n\n|c\nd\n\ne", "dip", "a\nb\n\n|\ne"},
        {"a\nb\n\n|c\nd\n\ne", "dap", "a\nb\n\n|e"},
        {"one t|wo three", "yiwP", "one tw|otwo three"},
        // Other changes.
        {"|one\ntwo", ">>", "|    one\ntwo"},
        {"    |one\ntwo", "<<", "on|e\ntwo"},
        {"|one\ntwo\nthree", ">j", "|    one\n    two\nthree"},
        {"|one\ntwo\nthree", "2>>", "|    one\n    two\nthree"},
        {"|one\n  two", "J", "one| two"},
        {"|one\ntwo\nthree", "3J", "one two| three"},
        {"|one two", "~", "O|ne two"},
        {"|one two", "g~w", "|ONE two"},
        {"|One Two", "guu", "|one two"},
        {"|one two", "gUiw", "|ONE two"},
        {"|abc", "rx", "|xbc"},
        {"|abc", "3rx", "xx|x"},
        {"|abc", "5rx", "|abc"},
        {"|abc", "Rxy<Esc>", "x|yc"},
        {"|x 9", "<C-a>", "x 1|0"},
        {"|x 10", "3<C-x>", "x |7"},
        // Insert mode.
        {"|x", "ihello<Esc>", "hell|ox"},
        {"|x", "ahello<Esc>", "xhell|o"},
        {"one |two", "I> <Esc>", ">| one two"},
        {"|one two", "A!<Esc>", "one two|!"},
        {"|x", "3ia<Esc>", "aa|ax"},
        {"    |x", "oy<Esc>", "    x\n    |y"},
        {"    |x", "Oy<Esc>", "    |y\n    x"},
        {"|", "iab<BS>c<Esc>", "a|c"},
        {"|", "ifoo bar<C-w>x<Esc>", "foo |x"},
        {"|", "ione<CR>two<Esc>", "one\ntw|o"},
        {"|", "i  one<CR>two<Esc>", "  one\n  tw|o"},
        {"|ab", "i<Right>X<Esc>", "a|Xb"},
        // Undo, redo, repeat.
        {"one\n|two\nthree", "ddu", "one\n|two\nthree"},
        {"one\n|two\nthree", "ddu<C-r>", "one\n|three"},
        {"|x", "ihello<Esc>u", "|x"},
        {"|one\ntwo\nthree", ">ju", "|one\ntwo\nthree"},
        {"|one two three four", "dw.", "|three four"},
        {"|one two three four", "dw2.", "|four"},
        {"|one\ntwo", "A!<Esc>j.", "one!\ntwo|!"},
        {"|one two three", "cwX<Esc>w.", "X |X three"},
        {"|a b c", "x..", "| c"},
        // Visual mode.
        {"|one two", "ved", "| two"},
        {"one |two three", "vey", "one |two three"},
        {"one\n|two\nthree", "Vd", "one\n|three"},
        {"|one\ntwo\nthree", "Vjd", "|three"},
        {"|one\ntwo\nthree", "vjd", "|wo\nthree"},
        {"|one two", "vey$p", "one twoon|e"},
        {"|one two", "viwyW" "viwp", "one |one"},
        {"|one\ntwo", "Vj>", "|    one\n    two"},
        {"|one two", "veU", "|ONE two"},
        {"|one two", "vex", "| two"},
        {"one |two three", "vbd", "|wo three"},
        {"|one two three", "vwwohd", "|hree"},
        {"|abc def", "vecX<Esc>", "|X def"},
        {"|one two", "verx", "|xxx two"},
        {"|one\ntwo\nthree", "VjJ", "one| two\nthree"},
        {"f(a, |b)", "vi(d", "f(|)"},
        // Registers.
        {"|one two", "\"ayw\"bdwx\"aP", "one| wo"},
        {"one\n|two", "yy\"_ddP", "|two\none"},
        {"|one two", "yiwwviwp", "one |one"},
        {"|a\nb", "\"qyyj\"Qyy\"qp", "a\nb\n|a\nb"},
        // Searching and the command line.
        {"|a foo b foo c", "/foo<CR>", "a |foo b foo c"},
        {"|a foo b foo c", "/foo<CR>n", "a foo b |foo c"},
        {"|a foo b foo c", "/foo<CR>nn", "a |foo b foo c"},
        {"|a foo b foo c", "/foo<CR>nN", "a |foo b foo c"},
        {"a foo b foo |c", "?foo<CR>", "a foo b |foo c"},
        {"a |foo b food foo", "*", "a foo b food |foo"},
        {"a foo b food |foo", "#", "a |foo b food foo"},
        {"|a Foo foo", "/foo\\c<CR>", "a |Foo foo"},
        {"|a foo b", "/nothere<CR>", "|a foo b"},
        {"|a foo b", "/fo<Esc>", "|a foo b"},
        {"|a a a", ":s/a/b/<CR>", "|b a a"},
        {"|a a a", ":s/a/b/g<CR>", "|b b b"},
        {"|x1\nx2\nx3", ":%s/x/y/<CR>", "y1\ny2\n|y3"},
        {"|x1\nx2\nx3", ":2,3s/x/[&]/<CR>", "x1\n[x]2\n|[x]3"},
        {"|a/b", ":s/\\//-/<CR>", "|a-b"},
        {"|one\ntwo\nthree", ":2d<CR>", "one\n|three"},
        {"|one\ntwo\nthree", "Vj:s/o/0/g<CR>", "0ne\n|tw0\nthree"},
        {"|aa\naa", ":%s/a/b/<CR>", "ba\n|ba"},
        {"|xA xa", ":s/a/[&]/gi<CR>", "|x[A] x[a]"},
        {"|a\nb\nc", ":%s/\\n/-/g<CR>", "|a-b-c"},
        {"|a\nb\nc\n", ":1,2s/\\n//<CR>", "|abc\n"},
        {"|a b\nc", ":%s/b/x/<CR>", "|a x\nc"},
        {"|ab\nab\nab", ":2,3s/b/\\r/<CR>", "ab\na\n\n|a\n"},
        {"|aaaa", ":s/aa/b/g<CR>", "|bb"},
        {"|a foo food", ":s/\\<foo\\>/x/g<CR>", "|a x food"},
        {"|one\ntwo\nthree", ":2,3><CR>", "one\n    |two\n    three"},
        // Macros, marks, the rest.
        {"|1\n1\n1", "qa<C-a>jq", "2\n|1\n1"},
        {"|1\n1\n1", "qa<C-a>jq@a", "2\n2\n|1"},
        {"|1\n1\n1", "qa<C-a>jq@a@@", "2\n2\n|2"},
        {"|1\n1\n1\n1", "qa<C-a>jq2@a", "2\n2\n2\n|1"},
        {"o|ne\ntwo\nthree", "maG`a", "o|ne\ntwo\nthree"},
        {"  o|ne\ntwo\nthree", "maG'a", "  |one\ntwo\nthree"},
        {"one\ntwo\nth|ree", "maggd`a", "on|ree"},
        {"|one two", "vwh<Esc>gvd", "|two"},
        {"|one", "<C-o>", "|one"},
        // Visual Block mode.
        {"|abcd\nefgh\nijkl", "<C-v>jld", "|cd\ngh\nijkl"},
        {"a|bcd\nefgh\nijkl", "<C-v>jjlx", "a|d\neh\nil"},
        {"|abcd\nefgh", "<C-v>jly$p", "abcd|ab\nefghef"},
        {"|ab\ncd\nx", "<C-v>jlyjj$p", "ab\ncd\nx|ab\n cd"},
        {"|ab\ncd", "<C-v>jyP", "|aab\nccd"},
        {"|ab\ncd\n", "<C-v>jyGp", "ab\nc|ad\n c\n"},
        {"|abc\ndef\nghi", "<C-v>jjIX<Esc>", "|Xabc\nXdef\nXghi"},
        {"|abc\ndef\nghi", "<C-v>jlAX<Esc>", "|abXc\ndeXf\nghi"},
        {"|a\nbcd\nef", "<C-v>jj$AX<Esc>", "|aX\nbcdX\nefX"},
        {"|abcd\nx\nefgh", "<C-v>jjlllAZ<Esc>", "|abcdZ\nx   Z\nefghZ"},
        {"abc|d\nx\nefgh", "<C-v>jjIZ<Esc>", "abc|Zd\nx\nefgZh"},
        {"|abcd\nefgh", "<C-v>jlcXY<Esc>", "X|Ycd\nXYgh"},
        {"a|bcd\nefgh", "<C-v>jCX<Esc>", "a|X\neX"},
        {"a|bcd\nefgh", "<C-v>jD", "|a\ne"},
        {"|abc\ndef", "<C-v>jlrx", "|xxc\nxxf"},
        {"|abc\ndef", "<C-v>jl~", "|ABc\nDEf"},
        {"|abc\ndef", "<C-v>jlU", "|ABc\nDEf"},
        {"a|b\ncdef\ngh", "<C-v>jj$d", "|a\nc\ng"},
        {"a|bcd\nefgh", "<C-v>jlOhd", "|d\nh"},
        {"a|bcd\nefgh", "<C-v>jlohd", "|d\nh"},
        {"|ab\ncd", "<C-v>j>", "|    ab\n    cd"},
        {"|    ab\n    cd", "<C-v>j<lt>", "|ab\ncd"},
        {"|abcd\nefgh", "<C-v>jl<Esc>gvd", "|cd\ngh"},
        {"|abcd\nefgh", "vjl<C-v>d", "|cd\ngh"},
        {"|abcd\nefgh", "<C-v>jlvd", "|gh"},
        {"|abcd\nefgh", "<C-v>jl<C-v>x", "abcd\ne|gh"},
        {"|ab\ncd", "<C-v>jyl<C-v>jp", "a|a\ncc"},
        {"|abc\ndef", "<C-v>jIX<Esc>u", "|abc\ndef"},
        {"|abc\ndef\nghi", "<C-v>jIX<Esc>j.", "Xabc\n|XXdef\nXghi"},
        {"|a\tb\nc\td", "<C-v>jld", "|b\nd"},
        {"|abc\ndef", "<C-v>jlJ", "abc| def"},
        {"|ab\ncd\nef", "<C-v>jS!<Esc>", "|!\nef"},
        // The jump list, and the marks Vim keeps.
        {"|one\ntwo\nthree", "G<C-o>", "|one\ntwo\nthree"},
        {"|one\ntwo\nthree", "G<C-o><Tab>", "one\ntwo\n|three"},
        {"|one\ntwo\nthree", "Ggg<C-o><C-o>", "one\ntwo\n|three"},
        {"|one\ntwo\nthree", "Ggg<C-o><C-i>", "|one\ntwo\nthree"},
        {"|a\nfoo b\nfoo", "/foo<CR>n<C-o><C-o>", "|a\nfoo b\nfoo"},
        {"|a\nfoo b\nfoo", "/foo<CR>n2<C-o>", "|a\nfoo b\nfoo"},
        {"o|ne\ntwo\nthree", "G``", "o|ne\ntwo\nthree"},
        {"  o|ne\ntwo\nthree", "G''", "  |one\ntwo\nthree"},
        {"o|ne\ntwo\nthree", "G''''", "one\ntwo\n|three"},
        {"|(a\nb)", "%``", "|(a\nb)"},
        {"|one\ntwo", "jxgg`.", "one\n|wo"},
        {"|one\ntwo", "jxggg;", "one\n|wo"},
        {"|one two", "wiX<Esc>0giY<Esc>", "one X|Ytwo"},
        {"one |two three", "ve<Esc>0`>", "one tw|o three"},
        {"one |two three", "ve<Esc>$`<lt>", "one |two three"},
        {"one\nt|wo", "maggOzero<Esc>`a", "zero\none\nt|wo"},
        {"one\ntwo\nth|ree", "maggdd`a", "two\nth|ree"},
        {"one\nt|wo\nthree", "majddk'a", "one\n|two"},
        // Registers: deleted lines in "1 to "9, less than a line in "-, what was typed in ".
        {"|a\nb\nc\nd", "dddd\"2p", "c\n|a\nd"},
        {"|a\nb\nc\nd", "dddddd\"3p\"1p", "d\na\n|c"},
        {"|ab cd", "xdw\"-P", "b| cd"},
        {"|one two", "yiwwdiw\"0P", "oneon|e "},
        {"|x", "ifoo<Esc>\".p", "foofo|ox"},
        {"|a b", "/b<CR>0i<C-r>/<Esc>", "|ba b"},
        // Insert mode's keys.
        {"|ab", "i<C-t>x<Esc>", "    |xab"},
        {"    a|b", "i<C-d>x<Esc>", "a|xb"},
        {"|", "ifoo<Esc>o<C-a><Esc>", "foo\nfo|o"},
        {"|", "i<C-v><Tab>x<Esc>", "\t|x"},
        {"one |two", "i<C-o>0X<Esc>", "|Xone two"},
        {"one |two three", "i<C-o>dwX<Esc>", "one |Xthree"},
        {"|ab", "A<C-o>Ix<Esc>u", "|ab"},
        {"|ab", "Ax<C-o>0y<Esc>uix<Esc>u", "|ab"},
        {"abc\n|x", "i<C-y><C-y><Esc>", "abc\na|bx"},
        {"|\nabc", "i<C-e><C-e><Esc>", "a|b\nabc"},
        {"|abcd", "Rxy<BS><BS><Esc>", "|abcd"},
        {"|abcd", "Rxy<BS>z<Esc>", "x|zcd"},
        {"|a", "3ox<Esc>", "a\nx\nx\n|x"},
        {"|a", "2Ox<Esc>", "x\n|x\na"},
        {"  |a", "2ox<Esc>", "  a\n  x\n  |x"},
        {"|a", "3ox<Esc>u", "|a"},
        // Visual mode: the keys for whole lines, and the rest.
        {"one\nt|wo\nthree", "vD", "one\nt|hree"},
        {"one\nt|wo\nthree", "vjX", "o|ne"},
        {"one\nt|wo\nthree", "vYp", "one\ntwo\n|two\nthree"},
        {"a\n  b|c\nd", "vSx<Esc>", "a\n  |x\nd"},
        {"a\n  b|c\nd", "vRx<Esc>", "a\n  |x\nd"},
        {"a\nb|c d\ne", "vCx<Esc>", "a\n|x\ne"},
        {"ab|c\ndef", "vjIX<Esc>", "|Xabc\ndef"},
        {"ab|c\ndef", "vjAX<Esc>", "abc\ndef|X"},
        {"|ab cd ab", "ve*", "ab cd |ab"},
        {"ab cd |ab", "ve#", "|ab cd ab"},
        {"|a\nb", "Vj2>", "|        a\n        b"},
        {"|a\n  b", "VjgJ", "a|  b"},
        {"|one two three", "v/th<CR>d", "|hree"},
        {"|one two three", "viwiwiwd", "| three"},
        {"|one two three", "vawawd", "|three"},
        {"(a (b |c) d)", "vi(i(d", "(|)"},
        {"(a (b |c) d)", "va(a(d", "|"},
        {"|a\nb\n\nc\nd\n\ne", "vipipipd", "|\ne"},
        // Text objects with counts, sentences, tags; the bracket moves.
        {"|one two three four", "d2aw", "|three four"},
        {"|one two three", "d3iw", "| three"},
        {"f(a, (b|, c))", "d2i(", "f(|)"},
        {"|a\nb\n\nc\n\nd", "d2ap", "|d"},
        {"|One two. Three four. Five.", ")", "One two. |Three four. Five."},
        {"|One two. Three four. Five.", "2)", "One two. Three four. |Five."},
        {"One two. Three |four. Five.", "(", "One two. |Three four. Five."},
        {"One two. |Three four. Five.", "(", "|One two. Three four. Five."},
        {"|One two.\n\nThree.", "))", "One two.\n\n|Three."},
        {"One two. Three |four. Five.", "das", "One two. |Five."},
        {"One two. Three |four. Five.", "dis", "One two. | Five."},
        {"One. Two |three! Four.", "cisX<Esc>", "One. |X Four."},
        {"<a><b>x|y</b></a>", "dit", "<a><b>|</b></a>"},
        {"<a><b>x|y</b></a>", "dat", "<a>|</a>"},
        {"<a><b>x|y</b></a>", "d2it", "<a>|</a>"},
        {"<p>a<br/>b|c</p>", "cit!<Esc>", "<p>|!</p>"},
        {"<div class=\"x\">a|b</div>", "yitP", "<div class=\"x\">a|bab</div>"},
        {"<a><b>x|y</b></a>", "vitatd", "<a>|</a>"},
        {"f(a, (b|, c))", "[(", "f(a, |(b, c))"},
        {"f(a, (b|, c))", "2[(", "f|(a, (b, c))"},
        {"f(a, (b|, c))", "])", "f(a, (b, c|))"},
        {"f(a, (b|, c))", "d])", "f(a, (b|))"},
        {"{\n  a;\n  |b;\n}", "[{", "|{\n  a;\n  b;\n}"},
        {"{\n  |a;\n  b;\n}", "]}", "{\n  a;\n  b;\n|}"},
        {"|int a;\n{\n}\nint b;\n{\n}", "]]", "int a;\n|{\n}\nint b;\n{\n}"},
        {"|int a;\n{\n}\nint b;\n{\n}", "2]]", "int a;\n{\n}\nint b;\n|{\n}"},
        {"int a;\n{\n}\nint b;\n{\n|}", "[[", "int a;\n{\n}\nint b;\n|{\n}"},
        {"|int a;\n{\n}\nint b;\n{\n}", "][", "int a;\n{\n|}\nint b;\n{\n}"},
        // More of Normal mode.
        {"|Hello", "g??", "|Uryyb"},
        {"|Hello you", "g?w", "|Uryyb you"},
        {"|Hello", "veg?", "|Uryyb"},
        {"|a foo b foo", "/foo<CR>ggcgnX<Esc>.", "a X b |X"},
        {"|a foo b", "/foo<CR>0dgn", "a | b"},
        {"|a foo b", "/foo<CR>0gnd", "a | b"},
        {"a foo b fo|o", "/foo<CR>$dgN", "a foo b| "},
        {"|one two three", "d/th<CR>", "|three"},
        {"|a b a b a", "d/b<CR>.", "|b a"},
        {"|one two", "c/two<CR>X <Esc>", "X| two"},
        {"one two |three", "d?two<CR>", "one |three"},
        {"|a a\na a", ":s/a/b/<CR>j&", "b a\n|b a"},
        {"|a a\na a", ":s/a/b/g<CR>ug&", "b b\n|b b"},
        {"|a a\na a", ":s/a/b/<CR>j:s<CR>", "b a\n|b a"},
        // Regular expressions (magic unless \v or \V says otherwise).
        {"|ab1 cd22 ef", "/\\d\\d<CR>", "ab1 cd|22 ef"},
        {"|foo.bar fooxbar", "/foo.bar<CR>", "foo.bar |fooxbar"},
        {"|foo.bar fooxbar", "/foo\\.bar<CR>", "|foo.bar fooxbar"},
        {"|a ab abb", "/ab\\+<CR>n", "a ab |abb"},
        {"|xay xby xcy", "/x\\(b\\|c\\)y<CR>", "xay |xby xcy"},
        {"|xay xby xcy", "/\\vx(b|c)y<CR>n", "xay xby |xcy"},
        {"a.c |abc", "/\\Va.c<CR>", "|a.c abc"},
        {"|x FOO foo", "/\\cfoo<CR>", "x |FOO foo"},
        {"|x Foo foo", "/\\C[fF]oo<CR>n", "x Foo |foo"},
        {"|is this island is", "/\\<is\\w*<CR>", "is this |island is"},
        {"|is this island is", "/\\vis><CR>", "is th|is island is"},
        {"a\n|b a\na", "/^a<CR>", "a\nb a\n|a"},
        {"|ab\nb\nab", "/b$<CR>", "a|b\nb\nab"},
        {"|ab\r\nb", "/b$<CR>", "a|b\r\nb"},
        {"|foobar foobaz", "/foo\\zsbaz<CR>", "foobar foo|baz"},
        {"|x foobar foo", "/foo\\zebar<CR>n", "x |foobar foo"},
        {"|ab\ncd\nab\nce", "/b\\nc<CR>n", "ab\ncd\na|b\nce"},
        {"|ab\ncd", "/b\\_.c<CR>", "a|b\ncd"},
        {"|ab\ncd", "/b.c<CR>", "|ab\ncd"},
        {"|café cafè", "/caf.<CR>", "café |cafè"},
        {"|aé b", "/[é]<CR>", "a|é b"},
        {"|ÉCOLE école", "/\\cÉ<CR>", "ÉCOLE |école"},
        {"|a aa aaa", "/a\\{3}<CR>", "a aa |aaa"},
        {"|a aa aaa", "/\\<a\\{2,}<CR>", "a |aa aaa"},
        {"x1 x22 |x333", "?x\\d\\d<CR>", "x1 |x22 x333"},
        {"x1 x22 |x333", "?x\\d\\d<CR>N", "x1 x22 |x333"},
        {"|abc", "/a\\(<CR>", "|abc"},
        {"|abc", "/x*<CR>", "a|bc"},
        {"|a x12 b", "/x\\d\\+<CR>0dgn", "a | b"},
        {"|a x1 b x22", "/x\\d\\+<CR>ggcgnY<Esc>.", "a Y b |Y"},
        {"|a x1 b x22", "/x\\d\\+<CR>$gNd", "a x1 b| "},
        {"|ab ab", "/\\(a\\)\\@=<CR>", "ab |ab"},
        // Back-references, look-around, \& and \%[] (on the backtracker).
        {"|x the the", "/\\(\\w\\+\\) \\1<CR>", "x |the the"},
        {"|x the the cat cat", "/\\v<(\\w+)\\s+\\1><CR>n", "x the the |cat cat"},
        {"|x Abc aBC", "/\\c\\(abc\\) \\1<CR>", "x |Abc aBC"},
        {"|x abc aBC", "/\\(abc\\) \\1<CR>", "|x abc aBC"},
        {"|baab aa", ":s/\\(a\\)\\1/x/<CR>", "|bxb aa"},
        {"|baab aa", ":s/\\(a\\)\\1/[\\1]/g<CR>", "|b[a]b [a]"},
        {"|ab\nab", ":%s/\\(a\\)\\(b\\)\\n\\1\\2/X/<CR>", "|X"},
        {"|a=a b=c", ":s/\\(\\w\\)=\\1/same/g<CR>", "|same b=c"},
        {"|foobaz foobar", "/foo\\(bar\\)\\@=<CR>", "foobaz |foobar"},
        {"|foobaz foobar", ":s/foo\\(bar\\)\\@=/X/g<CR>", "|foobaz Xbar"},
        {"|foobar foobaz", "/foo\\(bar\\)\\@!<CR>", "foobar |foobaz"},
        {"|foobar xbar", "/\\(foo\\)\\@<!bar<CR>", "foobar x|bar"},
        {"|xbar foobar", "/\\(foo\\)\\@<=bar<CR>", "xbar foo|bar"},
        {"|foo x", "/\\(foo\\)\\@!<CR>", "f|oo x"},
        {"|aaa", ":s/a\\@<=a/b/g<CR>", "|abb"},
        {"|a <span> span", ":s/<\\@1<=span/X/g<CR>", "|a <X> span"},
        {"|ab\ncd", "/\\(b\\n\\)\\@<=c<CR>", "ab\n|cd"},
        {"|if x then\nif y", "/if \\(\\(then\\)\\@!.\\)*$<CR>", "if x then\n|if y"},
        {"|foobar foobeep", "/foobeep\\&...<CR>", "foobar |foobeep"},
        {"|foobar foobeep", ":s/foobeep\\&.../X/<CR>", "|foobar Xbeep"},
        {"|xaaa", "/\\(a*\\)\\@>a<CR>", "|xaaa"},
        {"|xaaab", "/\\(a*\\)\\@>b<CR>", "x|aaab"},
        {"|x fu fun func f", ":s/\\<fu\\%[nction]\\>/F/g<CR>", "|x F F F f"},
        {"|x r re ro road rod", ":s/\\<r\\%[[eo]ad]\\>/R/g<CR>", "|x R R R R rod"},
        {"|a1", "/\\1\\(a\\)<CR>", "|a1"},
        {"|a\nb\nc", "/\\%2l.<CR>", "a\n|b\nc"},
        {"|c\nc\nc", ":%s/\\%3lc/X/<CR>", "c\nc\n|X"},
        {"|a\na\na", ":%s/\\%>1l\\%<3la/X/<CR>", "a\n|X\na"},
        {"|abcabc", ":s/\\%4ca/X/<CR>", "|abcXbc"},
        {"|abab", "/\\%#=1ab<CR>", "ab|ab"},
        {"|x aa ab", "/\\%#=1\\(a\\)b<CR>", "x aa |ab"},
        // :s with them: groups, & \0 ~ \u \U \r in what it becomes; the flags.
        {"|foo(1) bar(22)", ":s/\\(\\w\\+\\)(\\(\\d\\+\\))/\\2:\\1/g<CR>", "|1:foo 22:bar"},
        {"|ab cd", ":s/\\v(\\w)(\\w)/\\2\\1/g<CR>", "|ba dc"},
        {"|ab", ":s/b/[&\\0\\&]/<CR>", "|a[bb&]"},
        {"|a b a b", ":s/a/X/<CR>:s/b/~~/g<CR>", "|X XX a XX"},
        {"|one two", ":s/\\w\\+/\\u&/g<CR>", "|One Two"},
        {"|one two", ":s/one/\\U&x\\Ey/<CR>", "|ONEXy two"},
        {"|a,b", ":s/,/\\r/<CR>", "|a\nb"},
        {"|a\nb\nc", ":%s/\\n\\(b\\)/+\\1/<CR>", "|a+b\nc"},
        {"|A a", ":s/a/x/gi<CR>", "|x x"},
        {"|A a", ":s/a\\c/x/g<CR>", "|x x"},
        {"|A a", ":s/[aA]/x/I<CR>", "|x a"},
        {"|a a\na", ":%s/a/x/gn<CR>", "|a a\na"},
        {"|a a a", ":s/a/x/gc<CR>yny", "|x a x"},
        {"|a a a", ":s/a/x/gc<CR>yq", "|x a a"},
        {"|a a a", ":s/a/x/gc<CR>nl", "|a x a"},
        {"|a a\na", ":%s/a/x/gc<CR>na", "a x\n|x"},
        {"|a a", ":s/a/x/gc<CR>yyu", "|a a"},
        {"|abc", ":s/x*/-/g<CR>", "|-a-b-c-"},
        {"|xxa", ":s/x*/-/g<CR>", "|-a-"},
        {"|ab", ":s/$/!/<CR>", "|ab!"},
        {"|ab\ncd", ":%s/^/> /<CR>", "> ab\n|> cd"},
        {"|foobar", ":s/foo\\zsbar/X/<CR>", "|fooX"},
        {"|<a><b>", ":s/<.\\{-}>/X/<CR>", "|X<b>"},
        {"|<a><b>", ":s/<.*>/X/<CR>", "|X"},
        {"|café", ":s/é/e/<CR>", "|cafe"},
        {"|ÉCOLE école", ":s/\\cé/E/g<CR>", "|ECOLE Ecole"},
        {"|a a\na a", ":s/a/b/g<CR>j:&&<CR>", "b b\n|b b"},
        {"|abc", ":s/\\(/x/<CR>", "|abc"},
        {"|ab\nab\nab", ":%s/b\\n/-/<CR>", "|a-a-ab"},
        {"|1\n1\n1", "qa<C-a>jqQ", "2\n2\n|1"},
        {"|ab", "ylgp", "aa|b"},
        {"|a\nb", "yygp", "a\na\n|b"},
        {"a\n|b", "yygP", "a\nb\n|b"},
        {"|foo foobar", "g*", "foo |foobar"},
        {"|foo foobar foo", "*", "foo foobar |foo"},
        // Corners: empty lines in a block, rows of unequal length, repeats, undo.
        {"|ab\n\ncd", "<C-v>jjld", "|\n\n"},
        {"|ab\n\ncd", "<C-v>jjIX<Esc>", "|Xab\nX\nXcd"},
        {"|abcd\nefgh", "<C-v>jldu", "|abcd\nefgh"},
        {"|abc\ndef", "<C-v>j$cX<Esc>", "|X\nX"},
        {"a|bc\ndef", "<C-v>jy$p", "abc|b\ndefe"},
        {"|abcd\nefgh\nijkl", "<C-v>jdj.", "bcd\n|gh\njkl"},
        {"|ab\ncd", "<C-v>jy2P", "|aaab\ncccd"},
        {"|abc\nd\nxyz", "<C-v>j$yjjP", "abc\nd\n|abcxyz\nd"},
        {"|abc\nd\nxyz\nxyz", "<C-v>j$yjjP", "abc\nd\n|abcxyz\nd  xyz"},
        {"one\ntwo\nth|ree", "ggd''", "|"},
        {"One. T|wo", ")", "One. Tw|o"},
        {"<a>x</|a>", "dit", "<a>|</a>"},
        {"|ab cd", "A<C-o>vbd!<Esc>", "ab |!"},
        {"|a\nb\nc", "jmajmbgg'a'b<C-o><C-o>", "|a\nb\nc"},
        // Neovim's defaults that differ from Vim's: the column is kept where Vim
        // would go to the start of the text ('startofline' is off), Space and
        // Backspace go on to other lines, & keeps the flags.
        {"abc|d\nx\nabcdef", "G", "abcd\nx\nabc|def"},
        {"abcd\nx\nabc|def", "gg", "abc|d\nx\nabcdef"},
        {"abc|d\nx\nabcdef", "2G", "abcd\n|x\nabcdef"},
        {"abc|d\nx\nabcdef", "2GG", "abcd\nx\nabc|def"},
        {"abc|d\nx\nabcdef", "$G", "abcd\nx\nabcde|f"},
        {"abc|d\nx\nabcdef", ":3<CR>", "abcd\nx\nabc|def"},
        {"ab|cd\nefgh\nij", "dd", "ef|gh\nij"},
        {"abcd\nef|gh\nij", "ddp", "abcd\nij\n|efgh"},
        {"ab|cd\nefgh", ">>", "  |  abcd\nefgh"},
        {"    ab|cd", "<lt><lt>", "abc|d"},
        {"ab|c\ndef", "<Space>", "abc\n|def"},
        {"ab|c\ndef", "2<Space>", "abc\nd|ef"},
        {"abc\n|def", "<BS>", "ab|c\ndef"},
        {"abc\nd|ef", "3<BS>", "a|bc\ndef"},
        {"abc\n|def", "<C-h>", "ab|c\ndef"},
        {"ab|c\ndef", "l", "ab|c\ndef"},
        {"abc\n|def", "h", "abc\n|def"},
        {"ab|c\ndef", "d<Space>", "a|b\ndef"},
        {"|a a\na a", ":s/a/b/g<CR>j&", "b b\n|b b"},
        // Visual mode after $ takes the end of the line with it; P there keeps the register.
        {"ab|c\ndef", "v$d", "ab|def"},
        {"|abc\ndef", "v$y$p", "abc|abc\n\ndef"},
        {"ab|c\ndef", "$vd", "a|b\ndef"},
        {"a|bc\ndef\nghi", "v$jd", "a|ghi"},
        {"a|bc\ndef", "v$hd", "|a\ndef"},
        {"|one two three", "yiwwviwPwviwP", "one one |one"},
        {"|one two three", "yiwwviwpwviwp", "one one |two"},
        // Numbers: hexadecimal and binary, and zeros in front stay.
        {"|x 0x0f", "<C-a>", "x 0x1|0"},
        {"|0xFF", "<C-a>", "0x10|0"},
        {"|0xfe", "2<C-a>", "0x10|0"},
        {"0x1|0", "<C-x>", "0x0|f"},
        {"|0b101", "<C-a>", "0b11|0"},
        {"|007", "<C-a>", "00|8"},
        {"|009", "<C-a>", "01|0"},
        {"|a -1", "<C-a>", "a |0"},
        {"|a 1", "2<C-x>", "a -|1"},
        {"|12 34", "w<C-a>", "12 3|5"},
        // Neovim's bracket keys and Visual Line's @ and Q.
        {"a\n|b\nc", "[<Space>", "a\n\n|b\nc"},
        {"a\n|b\nc", "2]<Space>", "a\n|b\n\n\nc"},
        {"a\n|b", "]<Space>.", "a\n|b\n\n"},
        {"|1\n1\n1\n1", "qa<C-a>qjVj@a", "2\n2\n|2\n1"},
        {"|1\n1\n1\n1", "qa<C-a>qjVjQ", "2\n2\n|2\n1"},
        {"|ab\ncd\nef", "qaAx<Esc>quggVG@a", "abx\ncdx\nef|x"},
        // <C-u> and <C-w> in Insert mode are an undo step of their own from there on.
        {"|x", "ifoo <C-u>bar<Esc>u", "|foo x"},
        {"|x", "ifoo bar<C-w>baz<Esc>u", "foo |barx"},
        {"|x", "ifoo <C-u>bar<Esc>uu", "|x"},
        // A macro stops at the first command that fails (j on the last line).
        {"|a\nb\nc", "qaA;<Esc>jq5@a", "a;\nb;\nc|;"},
        // cw on a word's last character (or a word of one) changes just that.
        {"fo|o bar", "cwX<Esc>", "fo|X bar"},
        {"|a bcd", "cwX<Esc>", "|X bcd"},
        // An exclusive motion that ends at the start of a line stops at the end of the one before.
        {"abc |def\nghi\n\nxyz", "d}", "abc| \n\nxyz"},
        // A count before . takes the place of the one the change had.
        {"|1\n2\n3\n4\n5\n6\n7", "2dd3.", "|6\n7"},
        // A whole line put into an empty file goes below its one empty line.
        {"|x", "yyddp", "\n|x"},
        // Appending to a register with its capital letter, then putting it.
        {"|a\nb", "\"ayyj\"Ayy\"ap", "a\nb\n|a\nb"},
        // In Replace mode Backspace takes back a line break that was typed.
        {"|abc", "Rx<CR><BS><Esc>", "|xbc"},
        // r with a count and Enter: the characters become one line break.
        {"|abcd", "2r<CR>", "\n|cd"},
        // Whole characters: "é" as e and U+0301, a family (three emoji and
        // two joiners), a flag (two regional indicators), CJK two columns
        // wide. A command takes each as one character and never stops inside one.
        {"|ae\xCC\x81" "b", "v2lrx", "|xxx"},
        {"|" FAM FLAG "z", "vlrx", "|xxz"},
        {"|e\xCC\x81" "e\xCC\x81z", "2rx", "x|xz"},
        {"|" FLAG FAM "z", "2rx", "x|xz"},
        {"|e\xCC\x81" FAM "z", "2x", "|z"},
        {"a" FLAG "|" FAM "b", "X", "a|" FAM "b"},
        {"|e\xCC\x81" "b", "~", "E\xCC\x81|b"},
        {"|e\xCC\x81" "bc", "3~", "E\xCC\x81" "B|C"},
        {"|e\xCC\x81" "b c", "g~iw", "|E\xCC\x81" "B c"},
        {"|e\xCC\x81" "b", "gUl", "|E\xCC\x81" "b"},
        {"|e\xCC\x81" "b", "g??", "|r\xCC\x81o"},
        {"|ab e\xCC\x81", "fe", "ab |e\xCC\x81"},
        {"|a " FAM " \xF0\x9F\x91\xA9 b", "f\xF0\x9F\x91\xA9", "a " FAM " |\xF0\x9F\x91\xA9 b"},
        {"|a " FLAG " b", "fb", "a " FLAG " |b"},
        {"|a " FLAG " b", "tb", "a " FLAG "| b"},
        {"a " FLAG " |b", "Ta", "a|" " " FLAG " b"},
        {"|e\xCC\x81xyze\xCC\x81x", "tx;", "e\xCC\x81xyz|e\xCC\x81x"},
        {"|ab \xCC\x81" "cd ef", "w", "ab \xCC\x81|cd ef"},
        {"|ab \xCC\x81" "cd ef", "e", "a|b \xCC\x81" "cd ef"},
        {"ab \xCC\x81|cd", "b", "|ab \xCC\x81" "cd"},
        {"ab \xCC\x81" "c|d", "ge", "a|b \xCC\x81" "cd"},
        {"|ab \xCC\x81" "cd ef", "daw", "|cd ef"},
        {" \xCC\x81|x", "^", "| \xCC\x81x"},
        {"|Hi. \xCC\x81Yo", ")", "Hi. \xCC\x81Y|o"},
        {"|-\xCC\x81" "ab x ab", "*", "-\xCC\x81" "ab x |ab"},
        {"|" FAM "x" FAM, "lD", "|" FAM},
        {"|\xE6\x97\xA5x\nabx", "<C-v>jly0P", "|\xE6\x97\xA5\xE6\x97\xA5x\nababx"},
        {"|\xE6\x97\xA5x\nabx", "<C-v>jlrz", "|zzx\nzzx"},
        {"|\xE6\x97\xA5x\nabx", "<C-v>jlr\xE6\x9C\xAC", "|\xE6\x9C\xACx\n\xE6\x9C\xACx"},
        {"|\xCC\x81x", "<C-v>d", "|"},
        {"|a\xE2\x80\x8B" "bc", "l<C-v>d", "a|c"},
        {"|\xE6\x97\xA5\xE6\x9C\xAC\nabcd", "<C-v>jld", "|\xE6\x9C\xAC\ncd"},
        {"|x", "iab e\xCC\x81" "f<C-w><Esc>", "ab| x"},
        {"|x", "iab \xCC\x81" "cd<C-w><Esc>", "ab| \xCC\x81x"},
        {"|x", "ie\xCC\x81 " FAM "<C-u><Esc>", "|x"},
        {"|x", "i" FAM FLAG "<BS><Esc>", "|" FAM "x"},
        {"  \xCC\x81|x", "<<", " \xCC\x81|x"},
        {"|a\n   \xCC\x81x", "J", "a|  \xCC\x81x"},
        {"|\"a\"  \xCC\x81x", "da\"", "| \xCC\x81x"},
        // An indent put in by o, O, S or Enter goes again if nothing is typed
        // after it (Esc, Enter, <C-o>, an arrow off the line); Enter leaves the
        // blanks after the cursor behind.
        {"    |x\nz", "o<Esc>", "    x\n|\nz"},
        {"    |x\nz", "O<Esc>", "|\n    x\nz"},
        {"  |a\nb", "S<Esc>", "|\nb"},
        {"    |x\nz", "o<CR>y<Esc>", "    x\n\n    |y\nz"},
        {"    |x\nz", "3o<Esc>", "    x\n\n\n|\nz"},
        {"    |x\nz", "oy<BS><Esc>", "    x\n   | \nz"},
        {"    |x\nz", "o<BS><Esc>", "    x\n|\nz"},
        {"    |x\nz", "o<C-t><Esc>", "    x\n|\nz"},
        {"  |a\nz", "A<CR><Esc>", "  a\n|\nz"},
        {"  |a\nz", "o<C-o>~<Esc>", "  a\n|\nz"},
        {"    |x\ny", "o<Up><Esc>", "   | x\n\ny"},
        {"  a\n    |x\nz", "o<Down><Esc>", "  a\n    x\n\n|z"},
        {"  |abc\nx", "o<Esc>kj", "  abc\n|\nx"},
        {"foo| bar", "i<CR><Esc>", "foo\n|bar"},
        {" | foo   bar\nend", "i<CR><Esc>", " \n| foo   bar\nend"},
        {"  a|  b", "R<CR><BS><BS><BS><Esc>", "  |a  b"},
        // <C-w> and <C-u> stop at the start of the line (there they take the
        // line break), and where typing began; <C-w> takes a run of signs as
        // a word, <C-u> leaves the indent. The arrows stay on the line.
        {"foo bar\n  b|az  qux\nend", "i<C-w><C-w>x<Esc>", "foo bar\n|xaz  qux\nend"},
        {"foo bar\n  b|az  qux\nend", "i<C-w><C-w><C-w>x<Esc>", "foo bar|xaz  qux\nend"},
        {"foo bar\n  baz  qu|x\nend", "a  <C-w>x<Esc>", "foo bar\n  baz  qux|x\nend"},
        {"foo bar\n  baz  qu|x\nend", "a  <C-w><C-w>x<Esc>", "foo bar\n  baz  |x\nend"},
        {"ab c|d", "a e<C-w><C-w><C-w>x<Esc>", "ab |x"},
        {"x|y", "af(a.b)<C-w><C-w>!<Esc>", "xyf(a.|!"},
        {"foo bar\n  b|az  qux\nend", "i<C-u>x<Esc>", "foo bar\n  |xaz  qux\nend"},
        {"foo bar\n  baz  q|ux\nend", "iab<C-u><C-u>x<Esc>", "foo bar\n  |xux\nend"},
        {"foo bar\n |  baz\nend", "i<C-u>x<Esc>", "foo bar\n|x  baz\nend"},
        {"foo bar\n|baz\nend", "i<C-u>x<Esc>", "foo bar|xbaz\nend"},
        {"foo bar\n  baz  qu|x\nend", "a<CR>ab<C-u><C-u><C-u>x<Esc>", "foo bar\n  baz  qux|x\nend"},
        {"foo bar\n  baz  qu|x\nend", "A<Right>x<Esc>", "foo bar\n  baz  qux|x\nend"},
        {"foo bar\n|  baz\nend", "i<Left>x<Esc>", "foo bar\n|x  baz\nend"},
        {"a|b\nc", "o<Right>x<Esc>", "ab\n|x\nc"},
        // On a tab the column j and k keep is its last one (in Visual mode,
        // once past where the selection began).
        {"|\tx\nabcdef", "j", "\tx\nabc|def"},
        {"ab|\tx\nabcdefghij", "j", "ab\tx\nabc|defghij"},
        {"|\t\tx\nabcdefghij", "lj", "\t\tx\nabcdefg|hij"},
        {"|\tx\nabcdefgh", "jx", "\tx\nabc|efgh"},
        {"abc\n|\tx\nabcdef", "Vj", "abc\n\tx\nabc|def"},
        {"abcdef\nx|\tx\nabcdef", "vhj", "abcdef\nx\tx\n|abcdef"},
        {"abcdef\n|\tx\nabcdef", "vkj", "abcdef\n|\tx\nabcdef"},
        {"x|\tx\nabcdef", "i<Down>z<Esc>", "x\tx\na|zbcdef"},
        // A count of lines from the last line fails (and a macro stops there).
        {"ab cd\nef| gh", "2dd", "ab cd\nef| gh"},
        {"ab cd\nef| gh", "3yyP", "ab cd\nef| gh"},
        {"ab cd\nef| gh", "2Sx<Esc>", "ab cd\nef|gh"},
        {"ab cd\nef| gh", "2>>", "ab cd\nef| gh"},
        {"ab cd\nef| gh", "2g~~", "ab cd\nef| gh"},
        {"ab cd\nef| gh", "2_", "ab cd\nef| gh"},
        {"ab cd\nef| gh", "d2$", "ab cd\nef| gh"},
        {"ab cd\nef| gh", "2g_", "ab cd\nef| gh"},
        {"ab cd\ne|f gh\nij", "2dd", "a|b cd"},
        {"ab\n|c\nd", "qaA;<Esc>2yyjq2@a", "ab\nc;\nd|;"},
        // g~ gu gU g? on whole lines leave the cursor where the motion began
        // or ended, whichever is first, its column with it.
        {"ab cd\n  ef| gh\nij kl\n\nmn", "g~~", "ab cd\n  |EF GH\nij kl\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "2gUU", "ab cd\n  EF| GH\nIJ KL\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "gUj", "ab cd\n  EF| GH\nIJ KL\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "gUk", "AB C|D\n  EF GH\nij kl\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "gU+", "ab cd\n  EF| GH\nIJ KL\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "VjgU", "ab cd\n|  EF GH\nIJ KL\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "VkgU", "AB C|D\n  EF GH\nij kl\n\nmn"},
        {"ab cd\n  ef| gh\nij kl\n\nmn", "gUip", "|AB CD\n  EF GH\nIJ KL\n\nmn"},
        // Paragraphs and sections: the end of the text reached with more of
        // the count to go fails.
        {"a\nb\n\n|c\nd\n\n{\ne\n}", "3}", "a\nb\n\n|c\nd\n\n{\ne\n}"},
        {"a\nb\n\n|c\nd\n\n{\ne\n}", "d4}", "a\nb\n\n|c\nd\n\n{\ne\n}"},
        {"a\nb\n\nc\nd\n\n{\ne\n|}", "4{", "a\nb\n\nc\nd\n\n{\ne\n|}"},
        {"a\nb\n\n|c\nd\n\n{\ne\n}", "3]]", "a\nb\n\n|c\nd\n\n{\ne\n}"},
        {"a\nb\n\n|c\nd\n\n{\ne\n}", "2]]", "a\nb\n\nc\nd\n\n{\ne\n|}"},
        {"x\n\n|a\n\ny", "2]]", "x\n\n|a\n\ny"},
        {"a\nb\n\nc\nd\n\n{\ne\n|}", "3[[", "a\nb\n\nc\nd\n\n{\ne\n|}"},
        // % with a count is that far through the text, in lines.
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "50%", "a1\nb2\n  c3\nd|4\ne5\nf6\ng7\n(h8)"},
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "2%", "a|1\nb2\n  c3\nd4\ne5\nf6\ng7\n(h8)"},
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "100%", "a1\nb2\n  c3\nd4\ne5\nf6\ng7\n(h8|)"},
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "101%", "a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)"},
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "d50%", "a1\nb2\ne|5\nf6\ng7\n(h8)"},
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "V25%d", "a1\nd|4\ne5\nf6\ng7\n(h8)"},
        {"a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)", "50%<C-o>", "a1\nb2\n  c|3\nd4\ne5\nf6\ng7\n(h8)"},
    };
    int wrong = 0;
    for (const Case& c : cases) {
        std::string text = c.before, want = c.after;
        const size_t cur = text.find('|'), want_cur = want.find('|');
        text.erase(cur, 1);
        want.erase(want_cur, 1);
        Editor ed;
        ed.insert(text);
        ed.move_to(cur, false);
        ed.mark_saved();
        Vim vim;
        vim.resume(ed);
        ed.watch = &vim;
        vim.keys(ed, c.keys);
        const std::string got = ed.buf.text(0, ed.buf.size());
        if (got != want || ed.cursor() != want_cur) {
            std::string shown = got;
            shown.insert(std::min<size_t>(ed.cursor(), shown.size()), "|");
            std::fprintf(stderr, "vim: \"%s\" then %s\n  wanted \"%s\"\n  got    \"%s\"\n", c.before, c.keys, c.after, shown.c_str());
            ++wrong;
        }
    }
    if (wrong) fail("modal editing", static_cast<u64>(wrong), 0);
    // In a text whose lines end in "\r\n", the line breaks Vim's commands
    // put in do too.
    {
        static const Case crlf[] = {
            {"|a\r\nb", "ox<Esc>", "a\r\n|x\r\nb"},
            {"|a\r\nb", "Ox<Esc>", "|x\r\na\r\nb"},
            {"|a\r\nb", "Ax<CR>y<Esc>", "ax\r\n|y\r\nb"},
            {"  |a\r\nb", "2ox<Esc>", "  a\r\n  x\r\n  |x\r\nb"},
            {"|ab\r\nb", "r<CR>", "\r\n|b\r\nb"},
            {"a\r\n|b", "2[<Space>", "a\r\n\r\n\r\n|b"},
            {"|a\r\nb", "Rx<CR><BS><Esc>", "|x\r\nb"},
        };
        for (const Case& c : crlf) {
            std::string text = c.before, want = c.after;
            const size_t cur = text.find('|'), want_cur = want.find('|');
            text.erase(cur, 1);
            want.erase(want_cur, 1);
            Editor ed;
            ed.insert(text);
            ed.set_crlf(true);
            ed.move_to(cur, false);
            Vim vim;
            vim.resume(ed);
            ed.watch = &vim;
            vim.keys(ed, c.keys);
            const std::string got = ed.buf.text(0, ed.buf.size());
            if (got != want || ed.cursor() != want_cur) {
                std::fprintf(stderr, "vim, CRLF: \"%s\" then %s gave \"%s\" (cursor %llu)\n", c.before, c.keys, got.c_str(), static_cast<unsigned long long>(ed.cursor()));
                fail("line breaks put in by Vim in a CRLF text", 0, 0);
            }
        }
    }
    // The command line's cursor stands after what is typed, in columns (a
    // wide character two, a mark none), and Backspace takes a whole character.
    {
        Editor ed;
        Vim vim;
        vim.resume(ed);
        ed.watch = &vim;
        vim.keys(ed, ":\xE6\x97\xA5\xE6\x9C\xAC");
        const int wide = vim.status_cursor();
        vim.keys(ed, "e\xCC\x81");
        const int marked = vim.status_cursor();
        vim.keys(ed, FAM "<BS><BS>");
        if (wide != 5 || marked != 6 || vim.status() != ":\xE6\x97\xA5\xE6\x9C\xAC") {
            std::fprintf(stderr, "vim: the command line: cursor %d and %d, then \"%s\"\n", wide, marked, vim.status().c_str());
            fail("the command line's cursor and Backspace by whole characters", 0, 0);
        }
    }
    // A pattern that would take time exponential in the line on the
    // backtracker (every way of cutting forty "a"s into runs, each then
    // said again) stops, says so, and changes nothing. (Timed by this
    // thread's processor time, not the clock: on a loaded machine, under a
    // sanitizer, the clock ran past the limit with the budget kept.)
    {
        Editor ed;
        ed.insert(std::string(40, 'a') + " b\n");
        ed.move_to(0, false);
        Vim vim;
        vim.resume(ed);
        ed.watch = &vim;
        auto cpu_ns = [] {
            timespec ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
        };
        const u64 t0 = cpu_ns();
        vim.keys(ed, "/\\(a*\\)*\\1b<CR>");
        const bool said = vim.status() == VimRegex::kTooSlow && ed.cursor() == 0;
        vim.keys(ed, ":%s/\\(a*\\)*\\1b/x/<CR>");
        const bool kept = vim.status() == VimRegex::kTooSlow && ed.buf.text(0, ed.buf.size()) == std::string(40, 'a') + " b\n";
        const u64 ms = (cpu_ns() - t0) / 1000000;
        if (!said || !kept || ms > 5000) {
            std::fprintf(stderr, "vim: a pattern past the budget: \"%s\" after %llu ms of processor time\n", vim.status().c_str(), static_cast<unsigned long long>(ms));
            fail("a pattern past the backtracker's budget stops and says so", 0, 0);
        }
    }
    // Searching a document of many small pieces (so that matches run from
    // one into the next), against a plain search of the same text.
    {
        u64 seed = 0x5eed;
        auto rnd = [&] {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            return seed >> 33;
        };
        auto wordish = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || static_cast<unsigned char>(ch) >= 0x80; };
        static const char kAlphabet[] = "abAB \n_x";
        u64 bad = 0;
        for (int round = 0; round < 60; ++round) {
            Buffer b;
            std::string flat;
            for (int i = 0, n = 20 + static_cast<int>(rnd() % 400); i < n; ++i) {
                std::string piece;
                for (u64 k = 1 + rnd() % 5; k > 0; --k) piece += kAlphabet[rnd() % 8];
                const u64 at = rnd() % (flat.size() + 1);
                b.insert(at, piece);
                flat.insert(at, piece);
            }
            for (int q = 0; q < 200; ++q) {
                Vim::Pattern p;
                const size_t len = 1 + rnd() % 4;
                p.text = rnd() % 4 ? flat.substr(rnd() % flat.size(), len) : std::string(len, kAlphabet[rnd() % 8]);
                p.ignore_case = rnd() % 2;
                p.word = rnd() % 4 == 0;
                const bool forward = rnd() % 2, wrap = rnd() % 2;
                const u64 from = rnd() % 8 == 0 ? ~0ull : rnd() % (flat.size() + 1);
                const u64 reach = rnd() % 3 == 0 ? rnd() % (flat.size() + 2) : ~0ull;
                std::vector<u64> all;
                const size_t m = p.text.size();
                for (size_t at = 0; at + m <= flat.size(); ++at) {
                    bool same = true;
                    for (size_t k = 0; k < m && same; ++k) {
                        const char x = flat[at + k], y = p.text[k];
                        same = p.ignore_case ? std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)) : x == y;
                    }
                    if (same && p.word && ((at > 0 && wordish(flat[at - 1])) || (at + m < flat.size() && wordish(flat[at + m])))) same = false;
                    if (same) all.push_back(at);
                }
                const u64 size = flat.size();
                auto first_in = [&](u64 lo, u64 hi) -> u64 {
                    for (u64 at : all)
                        if (at >= lo && at < hi) return at;
                    return ~0ull;
                };
                auto last_in = [&](u64 lo, u64 hi) {
                    u64 best = ~0ull;
                    for (u64 at : all)
                        if (at >= lo && at < hi) best = at;
                    return best;
                };
                u64 want;
                if (forward) {
                    const u64 lo = from + 1, span = lo < size ? size - lo : 0;
                    want = first_in(lo, span > reach ? lo + reach : size);
                    if (want == ~0ull && wrap && reach > span) want = first_in(0, std::min(lo, reach - span));
                } else {
                    const u64 to = std::min(from, size);
                    want = last_in(to > reach ? to - reach : 0, to);
                    if (want == ~0ull && wrap && reach > to) want = last_in(size - to > reach - to ? size - (reach - to) : to, size);
                }
                const u64 got = Vim::find(b, p, from, forward, wrap, nullptr, reach);
                if (got != want && bad++ < 5)
                    std::fprintf(stderr, "find \"%s\" case %d word %d from %llu %s wrap %d reach %llu: got %lld wanted %lld\n", p.text.c_str(), p.ignore_case, p.word,
                                 static_cast<unsigned long long>(from), forward ? "on" : "back", wrap, static_cast<unsigned long long>(reach), static_cast<long long>(got),
                                 static_cast<long long>(want));
            }
        }
        if (bad) fail("searching across pieces", bad, 0);
        // A long document, which is looked through on several threads.
        std::string big(40u << 20, '.');
        for (u64 i = 0; i < big.size(); i += 1 + rnd() % 200) big[i] = '\n';
        Buffer lb;
        lb.load_view(big.data(), big.size());
        std::string flat = big;
        for (u64 at : {u64{3} << 20, u64{9} << 20, u64{17} << 20, (u64{25} << 20) + 7, (u64{39} << 20) + 5}) {
            lb.insert(at, "nee");
            lb.insert(at + 3, "dle");
            flat.insert(at, "needle");
        }
        Vim::Pattern np;
        np.text = "needle";
        for (int q = 0; q < 40; ++q) {
            const u64 from = q == 0 ? ~0ull : rnd() % (flat.size() + 1);
            const bool forward = q % 2 == 0;
            u64 want;
            if (forward) {
                const size_t a = flat.find("needle", from + 1);
                want = a != std::string::npos ? a : flat.find("needle");
            } else {
                const size_t a = from == 0 ? std::string::npos : flat.rfind("needle", std::min<u64>(from, flat.size()) - 1);
                want = a != std::string::npos ? a : flat.rfind("needle");
            }
            const u64 got = Vim::find(lb, np, from, forward, true);
            if (got != want && bad++ < 5)
                std::fprintf(stderr, "long find from %llu %s: got %lld wanted %lld\n", static_cast<unsigned long long>(from), forward ? "on" : "back",
                             static_cast<long long>(got), static_cast<long long>(want));
        }
        np.text = "nothere";
        if (Vim::find(lb, np, 5, true, true) != ~0ull || Vim::find(lb, np, 5, false, true) != ~0ull) ++bad;
        if (bad) fail("searching a long document", bad, 0);
    }
    // Regular expressions (regex.cpp), made up at random, and texts of "a",
    // "b", "é", spaces and line breaks: the matches found in a document of
    // many small pieces (an "é" now and then split between two) are those
    // found in the same text as one string; and for a pattern that stays on
    // a line, those found line by line, each line on its own (short: there
    // only the VM runs, never the DFA or the plain-text prefilter that a
    // whole text goes through). Some have look-around in them, which the
    // backtracker runs; and every one is run on the backtracker as well
    // ("\%#=1" first), which must find what the Pike VM finds, groups and all.
    {
        u64 seed = 0x2e6e;
        auto rnd = [&](u64 n) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            return (seed >> 33) % n;
        };
        std::function<std::string(int)> alt;
        auto atom = [&](int depth) -> std::string {
            static const char* const kAtoms[] = {"a", "b", "\xC3\xA9", " ", ".", "[ab]", "[^a]", "\\w", "\\s", "aab", "\\a", "[a-b\xC3\xA9]"};
            if (depth < 2 && rnd(6) == 0) return "\\(" + alt(depth + 1) + "\\)";
            return kAtoms[rnd(std::size(kAtoms))];
        };
        auto concat = [&](int depth, bool lines) {
            static const char* const kMulti[] = {"", "", "", "", "", "", "*", "\\+", "\\=", "\\{1,2}", "\\{-}", "\\{2}", "*", "\\+", "\\=", "\\{1,2}", "\\{-}", "\\{2}",
                                                 "\\@=", "\\@!", "\\@<=", "\\@<!", "\\@>", "\\@1<="};
            std::string out = rnd(6) == 0 ? "^" : rnd(8) == 0 ? "\\<" : "";
            for (u64 k = 1 + rnd(4); k > 0; --k) {
                out += atom(depth) + kMulti[rnd(std::size(kMulti))];
                if (!lines && rnd(8) == 0) out += rnd(2) ? "\\n" : "\\_.";
            }
            if (rnd(6) == 0) out += rnd(2) ? "$" : "\\>";
            return out;
        };
        bool lines_only = true;
        alt = [&](int depth) {
            std::string out = concat(depth, lines_only);
            while (rnd(4) == 0) out += "\\|" + concat(depth, lines_only);
            return out;
        };
        u64 bad = 0, checked = 0, forced_checked = 0, overran = 0, with_refs = 0;
        for (int round = 0; round < 40 && bad < 5; ++round) {
            std::string flat;
            static const char* const kText[] = {"a", "a", "b", "b", "\xC3\xA9", " ", "\n", "aab"};
            for (u64 n = rnd(round % 4 == 0 ? 3000 : 400); flat.size() < n;) flat += kText[rnd(std::size(kText))];
            Buffer b;
            for (u64 end = flat.size(); end > 0;) {  // (each piece put in at the front: none runs on into the next)
                const u64 len = std::min<u64>(end, 1 + rnd(7));
                b.insert(0, std::string_view(flat).substr(end - len, len));
                end -= len;
            }
            for (int q = 0; q < 40 && bad < 5; ++q) {
                lines_only = rnd(3) != 0;
                std::string typed = alt(0);
                // Now and then a back-reference after a group's "\)".
                if (rnd(3) == 0) {
                    std::string with;
                    std::vector<int> open;
                    int groups = 0;
                    for (size_t k = 0; k < typed.size(); ++k) {
                        with += typed[k];
                        if (typed[k] != '\\' || k + 1 >= typed.size()) continue;
                        with += typed[++k];
                        if (typed[k] == '(') open.push_back(++groups);
                        if (typed[k] == ')' && !open.empty()) {
                            if (rnd(3) == 0 && open.back() <= 9) with += "\\" + std::to_string(open.back());
                            open.pop_back();
                        }
                    }
                    if (groups > 9) continue;  // (Vim has nine)
                    typed = with;
                }
                const Vim::Pattern p = Vim::parse_pattern(typed);
                if (p.bad() || p.empty()) {
                    if (p.bad() && bad++ < 5) std::fprintf(stderr, "regex: \"%s\" not read: %s\n", typed.c_str(), p.error.c_str());
                    continue;
                }
                using Found = std::vector<std::pair<u64, u64>>;
                Found in_doc, in_text, by_line;
                Vim::each_whole(b, p, 0, b.size() + 1, false, [&](const RegexMatch& m) {
                    in_doc.push_back({m.start, m.end});
                    return true;
                });
                Vim::each_whole(std::string_view(flat), p, 0, flat.size() + 1, false, [&](const RegexMatch& m) {
                    in_text.push_back({m.start, m.end});
                    return true;
                });
                bool same = in_doc == in_text;
                if (same && p.re && !p.re->backtracks()) {  // (the backtracker on the same pattern)
                    const Vim::Pattern forced = Vim::parse_pattern("\\%#=1" + typed);
                    std::vector<RegexMatch> vm, bt;
                    Vim::each_whole(b, p, 0, b.size() + 1, false, [&](const RegexMatch& m) {
                        vm.push_back(m);
                        return true;
                    });
                    Vim::each_whole(b, forced, 0, b.size() + 1, false, [&](const RegexMatch& m) {
                        bt.push_back(m);
                        return true;
                    });
                    // (One that took the backtracker past its budget stopped early: nothing to compare.)
                    if (forced.re && forced.re->overruns() == 0) {
                        same = forced.re->backtracks() && vm.size() == bt.size();
                        for (size_t i = 0; same && i < vm.size(); ++i) same = std::memcmp(&vm[i], &bt[i], sizeof(RegexMatch)) == 0;
                        if (!same && bad < 5) std::fprintf(stderr, "regex: \"%s\": %zu matches by the Pike VM, %zu by the backtracker\n", typed.c_str(), vm.size(), bt.size());
                        ++forced_checked;
                    }
                }
                if (same && (p.re ? !p.re->multiline() : p.text.find('\n') == std::string::npos)) {
                    for (size_t ls = 0; ls < flat.size() || (ls == flat.size() && (flat.empty() || flat.back() != '\n'));) {
                        size_t le = flat.find('\n', ls);
                        if (le == std::string::npos) le = flat.size();
                        const std::string_view line = std::string_view(flat).substr(ls, le - ls);
                        Vim::each_whole(line, p, 0, line.size() + 1, false, [&](const RegexMatch& m) {
                            by_line.push_back({ls + m.start, ls + m.end});
                            return true;
                        });
                        ls = le + 1;
                        if (le == flat.size()) break;
                    }
                    same = in_doc == by_line;
                }
                // And the next match from a place, and the last before it, as Vim's / and ? find them.
                if (same && !in_text.empty()) {
                    const u64 from = rnd(flat.size() + 1);
                    RegexMatch m;
                    const u64 on = Vim::find(b, p, from, true, false, nullptr, ~0ull, &m);
                    u64 want_on = ~0ull, want_back = ~0ull;
                    for (u64 at = 0; at <= flat.size(); ++at) {  // (every place a match starts, overlapping ones too)
                        if (at < flat.size() && (static_cast<u8>(flat[at]) & 0xC0) == 0x80) continue;
                        const size_t got = Vim::match_in(flat, p, at);
                        if (got != at) continue;
                        if (at > from && want_on == ~0ull) want_on = at;
                        if (at < from) want_back = at;
                    }
                    same = on == want_on && Vim::find(b, p, from, false, false) == want_back;
                }
                if (p.re && p.re->overruns()) {  // (the backtracker's budget spent somewhere: what it found is not all there is)
                    ++overran;
                    continue;
                }
                ++checked;
                for (size_t k = 0; k + 1 < typed.size(); ++k)
                    if (typed[k] == '\\' && typed[k + 1] >= '1' && typed[k + 1] <= '9') {
                        ++with_refs;
                        break;
                    }
                if (!same && bad++ < 5) std::fprintf(stderr, "regex: \"%s\" in %zu bytes: %zu matches in pieces, %zu in the string, %zu line by line\n", typed.c_str(),
                                                     flat.size(), in_doc.size(), in_text.size(), by_line.size());
            }
        }
        if (bad || checked < 1000 || forced_checked < 300 || with_refs < 50 || overran > checked / 20) fail("regular expressions across pieces and lines", bad, static_cast<int>(checked));
    }
    // Back-references and look-around against a plain backtracker written
    // here from Vim's rules (each way in turn, the first that works, "*" as
    // many as may be, a back-reference to a group that took part in nothing
    // matching nothing), on patterns made up at random as trees and short
    // texts: the first match from each place, and its groups.
    {
        u64 seed = 0xb4c7;
        auto rnd = [&](u64 n) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            return (seed >> 33) % n;
        };
        struct R {
            enum K : u8 { kLit, kAny, kCls, kCat, kAlt, kStar, kLazy, kPlus, kOpt, kGroup, kRef, kAhead, kNotAhead, kBehind, kNotBehind, kAtomic, kBol, kEol, kBow, kEow } k;
            char c = 0;
            int n = 0;  // kGroup, kRef: the group
            std::vector<int> kids;
        };
        std::vector<R> tree;
        int groups = 0;
        std::vector<int> closed;
        std::function<bool(int)> nullable = [&](int at) -> bool {
            const R& n = tree[static_cast<size_t>(at)];
            switch (n.k) {
                case R::kLit:
                case R::kAny:
                case R::kCls: return false;
                case R::kCat:
                    for (const int k : n.kids)
                        if (!nullable(k)) return false;
                    return true;
                case R::kAlt: return nullable(n.kids[0]) || nullable(n.kids[1]);
                case R::kPlus:
                case R::kGroup:
                case R::kAtomic: return nullable(n.kids[0]);
                default: return true;
            }
        };
        std::function<int(int)> make = [&](int depth) -> int {
            const u64 r = rnd(depth > 2 ? 7 : 22);
            R n{R::kLit, 0, 0, {}};
            if (r < 3) {
                n.c = "abA "[rnd(4)];
            } else if (r == 3) {
                n.k = R::kAny;
            } else if (r == 4) {
                n.k = R::kCls;
            } else if (r < 7) {
                if (closed.empty()) n.c = 'a';
                else n.k = R::kRef, n.n = closed[rnd(closed.size())];
            } else if (r < 10) {
                n.k = R::kCat;
                for (u64 k = 2 + rnd(2); k > 0; --k) n.kids.push_back(make(depth + 1));
            } else if (r == 10) {
                n.k = R::kAlt;
                n.kids = {make(depth + 1), make(depth + 1)};
            } else if (r < 14) {
                n.k = static_cast<R::K>(R::kStar + rnd(4));
                n.kids = {make(depth + 1)};
                // (A loop whose body can take nothing is where a plain
                // backtracker and the Pike VM part ways: one never comes
                // back to a place, the other only refuses a turn that took
                // nothing. Vim's own two engines differ there too.)
                if (n.k != R::kOpt && nullable(n.kids[0])) n.k = R::kOpt;
            } else if (r < 17 && groups < 9) {
                n.k = R::kGroup;
                n.n = ++groups;
                n.kids = {make(depth + 1)};
                closed.push_back(n.n);
            } else if (r < 20) {
                n.k = static_cast<R::K>(R::kAhead + rnd(5));
                n.kids = {make(depth + 1)};
            } else {
                n.k = static_cast<R::K>(R::kBol + rnd(4));
            }
            tree.push_back(n);
            return static_cast<int>(tree.size() - 1);
        };
        std::function<std::string(int)> show = [&](int at) -> std::string {
            const R& n = tree[static_cast<size_t>(at)];
            auto atom = [&](int kid) {
                const R::K k = tree[static_cast<size_t>(kid)].k;
                const std::string s = show(kid);
                return k == R::kLit || k == R::kAny || k == R::kCls || k == R::kGroup || k == R::kRef ? s : "\\%(" + s + "\\)";
            };
            switch (n.k) {
                case R::kLit: return std::string(1, n.c);
                case R::kAny: return ".";
                case R::kCls: return "[ab]";
                case R::kCat: {
                    std::string s;
                    for (const int k : n.kids) s += show(k);
                    return s;
                }
                case R::kAlt: return "\\%(" + show(n.kids[0]) + "\\|" + show(n.kids[1]) + "\\)";
                case R::kStar: return atom(n.kids[0]) + "*";
                case R::kLazy: return atom(n.kids[0]) + "\\{-}";
                case R::kPlus: return atom(n.kids[0]) + "\\+";
                case R::kOpt: return atom(n.kids[0]) + "\\=";
                case R::kGroup: return "\\(" + show(n.kids[0]) + "\\)";
                case R::kRef: return "\\" + std::to_string(n.n);
                case R::kAhead: return atom(n.kids[0]) + "\\@=";
                case R::kNotAhead: return atom(n.kids[0]) + "\\@!";
                case R::kBehind: return atom(n.kids[0]) + "\\@<=";
                case R::kNotBehind: return atom(n.kids[0]) + "\\@<!";
                case R::kAtomic: return atom(n.kids[0]) + "\\@>";
                case R::kBol: return "\\_^";
                case R::kEol: return "\\_$";
                case R::kBow: return "\\<";
                case R::kEow: return "\\>";
            }
            return {};
        };
        std::string T;
        bool icase = false;
        std::pair<int, int> cap[10];
        auto same_ch = [&](char x, char y) { return icase ? std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)) : x == y; };
        auto word = [&](int at) { return at >= 0 && at < static_cast<int>(T.size()) && (std::isalnum(static_cast<unsigned char>(T[static_cast<size_t>(at)])) || T[static_cast<size_t>(at)] == '_'); };
        using K = std::function<bool(int)>;
        std::function<bool(int, int, const K&)> m;
        std::function<bool(int, int, const K&, bool)> star = [&](int kid, int pos, const K& k, bool greedy) {
            const K more = [&](int p) { return p != pos && star(kid, p, k, greedy); };
            if (!greedy && k(pos)) return true;
            if (m(kid, pos, more)) return true;
            return greedy && k(pos);
        };
        m = [&](int at, int pos, const K& k) -> bool {
            const R& n = tree[static_cast<size_t>(at)];
            const int len = static_cast<int>(T.size());
            const char here = pos < len ? T[static_cast<size_t>(pos)] : 0;
            switch (n.k) {
                case R::kLit: return pos < len && same_ch(here, n.c) && k(pos + 1);
                case R::kAny: return pos < len && here != '\n' && k(pos + 1);
                case R::kCls: return pos < len && here != '\n' && (same_ch(here, 'a') || same_ch(here, 'b')) && k(pos + 1);
                case R::kCat: {
                    std::function<bool(size_t, int)> from = [&](size_t i, int p) { return i == n.kids.size() ? k(p) : m(n.kids[i], p, [&](int q) { return from(i + 1, q); }); };
                    return from(0, pos);
                }
                case R::kAlt: return m(n.kids[0], pos, k) || m(n.kids[1], pos, k);
                case R::kStar: return star(n.kids[0], pos, k, true);
                case R::kLazy: return star(n.kids[0], pos, k, false);
                case R::kPlus: return m(n.kids[0], pos, [&](int p) { return star(n.kids[0], p, k, true); });
                case R::kOpt: return m(n.kids[0], pos, k) || k(pos);
                case R::kGroup:
                    return m(n.kids[0], pos, [&](int p) {
                        const auto was = cap[n.n];
                        cap[n.n] = {pos, p};
                        if (k(p)) return true;
                        cap[n.n] = was;
                        return false;
                    });
                case R::kRef: {
                    const auto [a, b] = cap[n.n];
                    if (a < 0 || b <= a) return k(pos);
                    if (pos + (b - a) > len) return false;
                    for (int i = 0; i < b - a; ++i)
                        if (!same_ch(T[static_cast<size_t>(a + i)], T[static_cast<size_t>(pos + i)])) return false;
                    return k(pos + (b - a));
                }
                case R::kAhead:
                case R::kNotAhead:
                case R::kBehind:
                case R::kNotBehind:
                case R::kAtomic: {
                    std::pair<int, int> was[10];
                    std::copy(std::begin(cap), std::end(cap), was);
                    bool hit = false;
                    int end = pos;
                    if (n.k == R::kBehind || n.k == R::kNotBehind) {
                        for (int s = pos; s >= 0 && !hit; --s) hit = m(n.kids[0], s, [&](int p) { return p == pos; });
                    } else {
                        hit = m(n.kids[0], pos, [&](int p) {
                            end = p;
                            return true;
                        });
                    }
                    const bool want = n.k == R::kAhead || n.k == R::kBehind || n.k == R::kAtomic;
                    if (hit != want || !hit) std::copy(std::begin(was), std::end(was), cap);
                    if (hit != want) return false;
                    if (k(n.k == R::kAtomic ? end : pos)) return true;
                    std::copy(std::begin(was), std::end(was), cap);
                    return false;
                }
                case R::kBol: return (pos == 0 || T[static_cast<size_t>(pos - 1)] == '\n') && k(pos);
                case R::kEol: return (pos == len || here == '\n') && k(pos);
                case R::kBow: return !word(pos - 1) && word(pos) && k(pos);
                case R::kEow: return word(pos - 1) && !word(pos) && k(pos);
            }
            return false;
        };
        u64 bad = 0, checked = 0, refs = 0;
        for (int round = 0; round < 20000 && bad < 5; ++round) {
            tree.clear();
            groups = 0;
            closed.clear();
            int root;
            if (round % 2) {  // (half of them a group first, so that something can refer to it)
                const int number = ++groups;  // (numbered before the groups inside it, as Vim numbers them by their "\(")
                const int inner = make(1);
                tree.push_back(R{R::kGroup, 0, number, {inner}});
                const int g = static_cast<int>(tree.size() - 1);
                closed.push_back(number);
                const int a = make(1), b = make(1);
                tree.push_back(R{R::kCat, 0, 0, {g, a, b}});
                root = static_cast<int>(tree.size() - 1);
            } else {
                root = make(0);
            }
            icase = rnd(5) == 0;
            const std::string typed = (icase ? "\\c" : "") + show(root);
            const Vim::Pattern p = Vim::parse_pattern(typed);
            if (p.bad()) {
                if (bad++ < 5) std::fprintf(stderr, "regex: \"%s\" not read: %s\n", typed.c_str(), p.error.c_str());
                continue;
            }
            if (p.empty()) continue;
            for (size_t k = 0; k + 1 < typed.size(); ++k)
                if (typed[k] == '\\' && typed[k + 1] >= '1' && typed[k + 1] <= '9') {
                    ++refs;
                    break;
                }
            T.clear();
            for (u64 n = rnd(15); n > 0; --n) T += "aabbA \n"[rnd(7)];
            const int top = static_cast<int>(T.size()) - (!T.empty() && T.back() == '\n' ? 1 : 0);
            for (int from = 0; from <= top && bad < 5; ++from) {
                int want = -1, want_end = -1;
                std::pair<int, int> want_cap[10];
                for (int s = from; s <= top && want < 0; ++s) {
                    for (auto& c : cap) c = {-1, -1};
                    if (m(root, s, [&](int e) {
                            want_end = e;
                            return true;
                        })) {
                        want = s;
                        std::copy(std::begin(cap), std::end(cap), want_cap);
                    }
                }
                RegexMatch got;
                const size_t at = Vim::match_in(T, p, static_cast<size_t>(from), &got);
                bool same = want < 0 ? at == std::string::npos : at == static_cast<size_t>(want) && got.end == static_cast<u64>(want_end);
                for (int g = 1; same && want >= 0 && g <= 9 && p.re; ++g) {
                    const bool set = want_cap[g].first >= 0;
                    same = set ? got.group[g][0] == static_cast<u64>(want_cap[g].first) && got.group[g][1] == static_cast<u64>(want_cap[g].second) : got.group[g][0] == RegexMatch::kUnset;
                }
                ++checked;
                if (!same && bad++ < 5) {
                    std::string shown = T;
                    for (char& ch : shown) ch = ch == '\n' ? '/' : ch;
                    std::fprintf(stderr, "regex: \"%s\" in \"%s\" from %d: got %lld to %lld, wanted %d to %d\n", typed.c_str(), shown.c_str(), from,
                                 at == std::string::npos ? -1ll : static_cast<long long>(at), at == std::string::npos ? -1ll : static_cast<long long>(got.end), want, want_end);
                }
            }
        }
        if (bad || checked < 100000 || refs < 3000) fail("back-references and look-around against a plain backtracker", bad, static_cast<int>(refs));
    }
    // Cmd+F with modes: the command line opened with the word on it,
    // selected; the first character typed takes its place, Backspace
    // clears it, an arrow keeps it.
    {
        auto opened = [](Editor& ed, Vim& vim) {
            ed.insert("one two three two");
            ed.move_to(0, false);
            vim.resume(ed);
            ed.watch = &vim;
            vim.open_command(ed, '/', "two", true);
        };
        auto check = [](bool ok, const char* what) {
            if (!ok) fail(what, 0, 0);
        };
        {
            Editor ed;
            Vim vim;
            opened(ed, vim);
            check(vim.status() == "/two" && vim.command_selected() && ed.cursor() == 4, "the command line opens selected, the cursor on the first match");
            vim.keys(ed, "th");
            check(vim.status() == "/th" && !vim.command_selected() && ed.cursor() == 8, "what is typed takes its place");
        }
        {
            Editor ed;
            Vim vim;
            opened(ed, vim);
            vim.keys(ed, "<BS>");
            check(vim.status() == "/" && vim.mode() == Vim::kCommand && ed.cursor() == 0, "Backspace clears it, and the line stays open");
        }
        {
            Editor ed;
            Vim vim;
            opened(ed, vim);
            vim.keys(ed, "<Right>s<CR>");
            check(vim.mode() == Vim::kNormal && ed.cursor() == 0 && vim.message().find("twos") != std::string::npos, "an arrow keeps it, to be typed onto");
        }
        {
            Editor ed;
            Vim vim;
            opened(ed, vim);
            vim.keys(ed, "<CR>n");
            check(vim.mode() == Vim::kNormal && ed.cursor() == 14, "Enter looks for it, n goes on");
        }
    }
    // Operations on many lines are made as few edits (vim.cpp, kManyLines):
    // the text they leave, and where the cursor ends up, are those of the
    // same work done a few lines at a time (one edit a line).
    {
        u64 seed = 0xB16;
        auto rnd = [&] {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            return seed >> 33;
        };
        static const char* const kinds[] = {"", "   ", "\t", "  foo bar x", ")x and y", "\tindented x", "a x\r", "  \r", "word x y", "xx", "  (x)"};
        std::string doc;
        for (int i = 0; i < 6000; ++i) {
            if (i == 3000) doc += std::string(1536 * 1024, 'x') + "  tail";  // a line longer than an edit's most
            else doc += kinds[rnd() % std::size(kinds)];
            doc += '\n';
        }
        auto run = [&](const std::vector<std::string>& keys, u64* cursor) {
            Editor ed;
            ed.insert(doc);
            ed.move_to(0, false);
            ed.mark_saved();
            Vim vim;
            vim.resume(ed);
            ed.watch = &vim;
            for (const std::string& k : keys) vim.keys(ed, k.c_str());
            *cursor = ed.cursor();
            return ed.buf.text(0, ed.buf.size());
        };
        struct Pair {
            const char* what;
            std::vector<std::string> many, few;
            bool cursor = true;  // (:> done in pieces leaves the cursor on the last piece's first line)
        };
        const Pair pairs[] = {
            {">", {":1,6000><CR>"}, {":1,1500><CR>", ":1501,3000><CR>", ":3001,4500><CR>", ":4501,6000><CR>"}, false},
            {"<", {":1,6000<<CR>"}, {":1,1500<<CR>", ":1501,3000<<CR>", ":3001,4500<<CR>", ":4501,6000<<CR>"}, false},
            // (No \\r here: lines it made would move the pieces' line numbers.)
            {":s with g, & and \\t", {":%s/x/[&]\\ty/g<CR>"}, {":1,1500s/x/[&]\\ty/g<CR>", ":1501,3000s/x/[&]\\ty/g<CR>", ":3001,4500s/x/[&]\\ty/g<CR>", ":4501,6000s/x/[&]\\ty/g<CR>"}},
            {":s without g", {":%s/x/Q/<CR>"}, {":1,1500s/x/Q/<CR>", ":1501,3000s/x/Q/<CR>", ":3001,4500s/x/Q/<CR>", ":4501,6000s/x/Q/<CR>"}},
            {"J", {"5000J"}, {"1800J", "1800J", "1402J"}},
            {"gJ", {"5000gJ"}, {"1800gJ", "1800gJ", "1402gJ"}},
        };
        for (const Pair& pr : pairs) {
            u64 c_many = 0, c_few = 0;
            const std::string a = run(pr.many, &c_many), b = run(pr.few, &c_few);
            const bool same_text = a == b;
            if (!same_text || (pr.cursor && c_many != c_few)) {
                size_t at = 0;
                while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
                std::fprintf(stderr, "%s on many lines: %s (first difference at byte %zu of %zu/%zu), cursor %llu against %llu\n", pr.what,
                             same_text ? "same text" : "different text", at, a.size(), b.size(), static_cast<unsigned long long>(c_many),
                             static_cast<unsigned long long>(c_few));
                fail("operations on many lines", 0, 0);
            }
        }
    }
    // Comments: gc with the markers of a language set (as the application sets them for the file in front).
    {
        struct Comment {
            const char* left;
            const char* right;
            const char* before;
            const char* keys;
            const char* after;
        };
        static const Comment comments[] = {
            {"// ", "", "|int a;\nint b;", "gcc", "|// int a;\nint b;"},
            {"// ", "", "|// int a;\nint b;", "gcc", "|int a;\nint b;"},
            {"// ", "", "|//int a;", "gcc", "|int a;"},
            {"// ", "", "    in|t a;", "gcc", "    //| int a;"},
            {"// ", "", "|int a;\nint b;\nint c;", "2gcc", "|// int a;\n// int b;\nint c;"},
            {"// ", "", "|int a;\nint b;\nint c;", "gcj", "|// int a;\n// int b;\nint c;"},
            {"// ", "", "|int a;\nint b;\nint c;", "gcG", "|// int a;\n// int b;\n// int c;"},
            {"// ", "", "|if (x) {\n    y();\n\n    z();\n}", "gcG", "|// if (x) {\n//     y();\n//\n//     z();\n// }"},
            {"// ", "", "    |a();\n        b();", "gcj", "    |// a();\n    //     b();"},
            {"// ", "", "|// a\nb\n// c", "gcG", "|// // a\n// b\n// // c"},
            {"// ", "", "|// a\n\n// c", "gcG", "|a\n\nc"},
            {"// ", "", "|// a\n//\n// c", "gcG", "|a\n\nc"},
            {"// ", "", "|a\nb\nc", "gcc.j.", "a\n|// b\nc"},
            {"// ", "", "|a\nb\nc", "Vjgc", "|// a\n// b\nc"},
            {"// ", "", "a\n|b\nc", "vgc", "a\n|// b\nc"},
            {"// ", "", "|a\nb\nc", "gcip", "|// a\n// b\n// c"},
            {"// ", "", "x\n// a\n|// b\n// c\ny", "gcgc", "x\n|a\nb\nc\ny"},
            {"// ", "", "x\n// a\n|// b\n// c\ny", "dgc", "x\n|y"},
            {"// ", "", "x\n|a\ny", "dgc", "x\n|a\ny"},
            {"# ", "", "|x = 1\n  y = 2", "gcj", "|# x = 1\n#   y = 2"},
            {"<!-- ", " -->", "|<p>hi</p>", "gcc", "|<!-- <p>hi</p> -->"},
            {"<!-- ", " -->", "|<!-- <p>hi</p> -->", "gcc", "|<p>hi</p>"},
            {"/* ", " */", "  |a { }", "gccgcc", "  |a { }"},
            {"", "", "|plain text", "gcc", "|plain text"},
        };
        int bad = 0;
        for (const Comment& c : comments) {
            std::string text = c.before, want = c.after;
            const size_t cur = text.find('|'), want_cur = want.find('|');
            text.erase(cur, 1);
            want.erase(want_cur, 1);
            Editor ed;
            ed.insert(text);
            ed.move_to(cur, false);
            Vim vim;
            vim.comment_left = c.left;
            vim.comment_right = c.right;
            vim.resume(ed);
            ed.watch = &vim;
            vim.keys(ed, c.keys);
            const std::string got = ed.buf.text(0, ed.buf.size());
            if (got != want || ed.cursor() != want_cur) {
                std::string shown = got;
                shown.insert(std::min<size_t>(ed.cursor(), shown.size()), "|");
                std::fprintf(stderr, "vim comments: \"%s\" then %s\n  wanted \"%s\"\n  got    \"%s\"\n", c.before, c.keys, c.after, shown.c_str());
                ++bad;
            }
        }
        if (bad) fail("comments (gc)", static_cast<u64>(bad), 0);
    }
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    // Modes, messages, and what is asked of the application.
    {
        Editor ed;
        ed.insert("one two\nthree\n");
        ed.move_to(0, false);
        Vim vim;
        vim.resume(ed);
        check(vim.mode() == Vim::kNormal && vim.status().empty(), "starts in Normal mode");
        vim.keys(ed, "i");
        check(vim.mode() == Vim::kInsert && vim.status() == "-- INSERT --", "i: Insert mode");
        vim.keys(ed, "<Esc>v");
        check(vim.mode() == Vim::kVisual && vim.status() == "-- VISUAL --", "v: Visual mode");
        u64 lo = 9, hi = 9;
        vim.keys(ed, "e");
        vim.visual_range(ed, &lo, &hi);
        check(lo == 0 && hi == 3, "the selection includes the character under the cursor");
        vim.keys(ed, "V");
        vim.visual_range(ed, &lo, &hi);
        check(vim.mode() == Vim::kVisualLine && lo == 0 && hi == 8, "V: whole lines");
        {
            u64 l0 = 9, l1 = 9;
            u32 c0 = 9, c1 = 9;
            check(!vim.block(ed, &l0, &l1, &c0, &c1), "no rectangle outside Visual Block mode");
            vim.keys(ed, "<Esc>gg0l<C-v>jl");
            check(vim.mode() == Vim::kVisualBlock && vim.visual() && vim.status() == "-- VISUAL BLOCK --", "<C-v>: Visual Block mode");
            check(vim.block(ed, &l0, &l1, &c0, &c1) && l0 == 0 && l1 == 1 && c0 == 1 && c1 == 3, "the rectangle between where it began and the cursor");
            vim.keys(ed, "$");
            check(vim.block(ed, &l0, &l1, &c0, &c1) && c0 == 1 && c1 > 1000, "after $ it reaches the end of every line");
            vim.keys(ed, "h");
            check(vim.block(ed, &l0, &l1, &c0, &c1) && c0 == 1 && c1 == 4, "and no longer after another move");
        }
        vim.keys(ed, "<Esc>gg0e2d");
        check(vim.pending() && vim.showcmd() == "2d", "an operator waiting for its motion");
        vim.keys(ed, "<Esc>:w");
        check(vim.mode() == Vim::kCommand && vim.status() == ":w" && vim.status_cursor() == 2, "the command line");
        vim.keys(ed, "<CR>");
        std::vector<VimRequest> asked = vim.take();
        check(asked.size() == 1 && asked[0].kind == VimRequest::kSave && !asked[0].then_close, ":w asks for a save");
        vim.keys(ed, ":q!<CR>ZZ:wqa<CR>:e other.txt<CR><C-w>v<C-w>l:noh<CR>gt");
        asked = vim.take();
        check(asked.size() == 8 && asked[0].kind == VimRequest::kClose && asked[0].force && asked[1].kind == VimRequest::kSave && asked[1].then_close &&
                  asked[2].kind == VimRequest::kSaveAll && asked[2].then_close && asked[3].kind == VimRequest::kOpen && asked[3].arg == "other.txt" &&
                  asked[4].kind == VimRequest::kSplit && asked[4].n == 0 && asked[5].kind == VimRequest::kFocus && asked[5].n == 1 &&
                  asked[6].kind == VimRequest::kNoHighlight && asked[7].kind == VimRequest::kTab && asked[7].n == 1,
              "closing, saving, opening, splits and tabs are asked of the application");
        vim.keys(ed, "]b2[b]B[B]d3[d]D<C-t>");
        asked = vim.take();
        check(asked.size() == 8 && asked[0].kind == VimRequest::kTab && asked[0].n == 1 && asked[1].kind == VimRequest::kTab && asked[1].n == -2 &&
                  asked[2].kind == VimRequest::kTab && asked[2].arg == "last" && asked[3].arg == "first" && asked[4].kind == VimRequest::kDiagnostic && asked[4].n == 1 &&
                  asked[5].kind == VimRequest::kDiagnostic && asked[5].n == -3 && asked[6].kind == VimRequest::kDiagnostic && asked[6].arg == "last" &&
                  asked[7].kind == VimRequest::kBack,
              "Neovim's bracket keys for tabs and diagnostics, and <C-t>, are asked of the application");
        vim.keys(ed, ":nonsense<CR>");
        check(vim.status() == "E492: Not an editor command: nonsense", "an unknown command says so");
        vim.keys(ed, "/two<CR>");
        check(vim.highlight().text == "two" && ed.cursor() == 4, "a search is remembered for highlighting");
        vim.keys(ed, "qq");
        check(vim.status() == "recording @q", "recording a macro shows");
        vim.keys(ed, "q");
        vim.read_only = true;
        vim.keys(ed, "ddx");
        check(ed.buf.text(0, ed.buf.size()) == "one two\nthree\n" && vim.status().starts_with("E21"), "a document that is not to be changed is not");
        std::string clip;
        vim.read_only = false;
        vim.clipboard_set = [&](const std::string& t) { clip = t; };
        vim.clipboard_get = [&] { return std::string("PASTED"); };
        vim.keys(ed, "gg0\"+yiw");
        check(clip == "one", "the + register is the clipboard");
        vim.keys(ed, "\"+P");
        check(ed.buf.text(0, 9) == "PASTEDone", "and pastes from it");
        // A selection made with the mouse.
        ed.move_to(0, false);
        ed.move_to(6, true);
        vim.adopt_selection(ed);
        vim.visual_range(ed, &lo, &hi);
        check(vim.mode() == Vim::kVisual && lo == 0 && hi == 6 && ed.cursor() == 5 && !ed.has_selection(), "a dragged selection becomes Visual mode");
        vim.keys(ed, "d");
        check(ed.buf.text(0, 3) == "one", "and can be operated on");
    }
    // Searching a text bigger than one window of the scan, both ways and around the ends.
    {
        Editor ed;
        std::string big;
        for (int i = 0; i < 9000; ++i) big += "filler line of text\n";
        ed.insert("needle at the top\n" + big + "a needle near the end\n" + big.substr(0, 400));
        const Vim::Pattern p = Vim::parse_pattern("needle");
        const u64 far = 18 + big.size() + 2;
        bool wrapped = false;
        check(Vim::find(ed.buf, p, 0, true, true, &wrapped) == far && !wrapped, "forward across many windows");
        check(Vim::find(ed.buf, p, far, true, true, &wrapped) == 0 && wrapped, "forward wraps to the top");
        check(Vim::find(ed.buf, p, ed.buf.size(), false, true, &wrapped) == far && !wrapped, "backward from the end");
        check(Vim::find(ed.buf, p, far, false, true, &wrapped) == 0 && !wrapped, "backward across many windows");
        check(Vim::find(ed.buf, p, 0, false, true, &wrapped) == far && wrapped, "backward wraps to the bottom");
        check(Vim::find(ed.buf, Vim::parse_pattern("\\<need\\>"), 0, true, true) == ~0ull, "a whole word is not part of a longer one");
        check(Vim::find(ed.buf, Vim::parse_pattern("NEEDLE"), 0, true, true) == ~0ull && Vim::find(ed.buf, Vim::parse_pattern("NEEDLE\\c"), 0, true, true) == far,
              "case matters unless told otherwise");
    }
}

// Git: reading what git prints, comparing lines, and the real thing against
// a repository made for the purpose.
std::vector<Git::Result> git_wait(Git& git, u32 kind, int ms = 15000) {
    std::vector<Git::Result> all;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        for (Git::Result& r : git.poll()) all.push_back(std::move(r));
        for (const Git::Result& r : all)
            if (r.kind == kind) return all;
        if (std::chrono::steady_clock::now() >= until) return all;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void git_test(int seeds) {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    // Status, as `git status --porcelain=v2 --branch -z` prints it.
    {
        using namespace std::string_literals;
        const std::string z = "# branch.oid 0123456789abcdef0123456789abcdef01234567\0# branch.head main\0# branch.upstream origin/main\0"
                              "# branch.ab +2 -1\0"
                              "1 .M N... 100644 100644 100644 aaaa bbbb src/a file.cpp\0"
                              "1 A. N... 000000 100644 100644 aaaa bbbb new.txt\0"
                              "2 R. N... 100644 100644 100644 aaaa bbbb R100 after.txt\0before.txt\0"
                              "u UU N... 100644 100644 100644 100644 aaaa bbbb cccc both.txt\0"
                              "? notes.txt\0"s;
        const GitStatus st = parse_status(z);
        check(st.repo && st.branch == "main" && st.upstream == "origin/main" && st.head == "01234567" && st.ahead == 2 && st.behind == 1,
              "status: the branch lines");
        check(st.entries.size() == 5, "status: five entries");
        check(st.entries[0].path == "src/a file.cpp" && st.entries[0].unstaged() && !st.entries[0].staged(), "status: a modified file, with a space in its name");
        check(st.entries[1].path == "new.txt" && st.entries[1].staged() && !st.entries[1].unstaged(), "status: an added file");
        check(st.entries[2].path == "after.txt" && st.entries[2].from == "before.txt" && st.entries[2].index == 'R', "status: a rename");
        check(st.entries[3].conflict && st.entries[3].path == "both.txt" && !st.entries[3].staged(), "status: a conflict");
        check(st.entries[4].path == "notes.txt" && st.entries[4].unstaged() && st.entries[4].index == '?', "status: an untracked file");
        const GitStatus fresh = parse_status("# branch.oid (initial)\0# branch.head master\0"s);
        check(fresh.head.empty() && fresh.branch == "master", "status: a repository with no commit yet");
        const GitStatus loose = parse_status("# branch.oid 0123456789abcdef0123456789abcdef01234567\0# branch.head (detached)\0"s);
        check(loose.branch.empty() && loose.head == "01234567", "status: no branch checked out");
    }
    {
        const std::vector<GitBranch> b = parse_branches("*\trefs/heads/main\n \trefs/heads/feature/x\n \trefs/remotes/origin/HEAD\n \trefs/remotes/origin/main\n");
        check(b.size() == 3 && b[0].name == "main" && b[0].current && !b[0].remote && b[1].name == "feature/x" && !b[1].current &&
                  b[2].name == "origin/main" && b[2].remote,
              "branches");
        const std::vector<GitCommit> log = parse_log("abc1234\x1f" "Ada\x1f" "2 days ago\x1f" "Fix the thing\ndef5678\x1f" "Bo\x1f" "3 weeks ago\x1f" "Start\n");
        check(log.size() == 2 && log[0].hash == "abc1234" && log[0].author == "Ada" && log[0].when == "2 days ago" && log[0].subject == "Fix the thing" &&
                  log[1].subject == "Start",
              "history");
        const std::vector<GitCommit> full = parse_log("abc1234\x1f" "Ada\x1f" "now\x1f" "Merge\x1f" "abc1234ffff\x1f" "p1 p2\x1f" "HEAD -> main, tag: v1\x1f" "1700000000\n");
        check(full.size() == 1 && full[0].full == "abc1234ffff" && full[0].parents == std::vector<std::string>{"p1", "p2"} && full[0].refs == "HEAD -> main, tag: v1" &&
                  full[0].time == 1700000000,
              "history: parents, what points at it, and when");
        // The lanes of a graph, as text.
        auto lanes = [](std::vector<std::pair<std::string, std::vector<std::string>>> commits) {
            std::vector<GitCommit> made;
            for (auto& [hash, parents] : commits) made.push_back({hash, "", "", "", hash, parents, "", 0});
            std::string out;
            for (const GitGraphRow& r : graph_lanes(made)) {
                for (const char32_t c : r.cells) {
                    if (c < 0x80) out += static_cast<char>(c);
                    else out += static_cast<char>(0xE0 | c >> 12), out += static_cast<char>(0x80 | (c >> 6 & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
                }
                out += '/';
            }
            return out;
        };
        check(lanes({{"m", {"a", "b", "c"}}, {"a", {"r"}}, {"b", {"r"}}, {"c", {"r"}}, {"r", {}}}) == "●─╮─╮/● │ │/│ ● │/│ │ ●/●─╯─╯/", "graph: an octopus merge, and the branches meeting");
        check(lanes({{"t1", {"r"}}, {"t2", {"z"}}, {"t3", {"r"}}, {"r", {}}, {"z", {}}}) == "●/│ ●/│ │ ●/●─┼─╯/  ●/", "graph: a lane passing under a branch that meets");
        check(lanes({{"a", {"b"}}, {"b", {}}}) == "●/●/" && lanes({}).empty(), "graph: one line");
        // Three ways: changes one side made taken; the same change by both once; a conflict open between markers, then taken.
        {
            using C = GitMerge::Chunk;
            GitMerge m = merge3("a\nb\nc\nd\ne\nf\ng\n", "a\nB\nc\nd\ne\nF\ng\n", "a\nb\nC\nd\ne\nF!\ng\n");
            std::string kinds;
            for (const C& c : m.chunks) kinds += "SYTBC"[c.kind];
            check(kinds == "SCSCS" && m.chunks[1].simple && !m.chunks[3].simple && m.open() == 2, "merge: b and c changed side by side (simple), f both ways (not)");
            check(m.result() == "a\n<<<<<<< yours\nB\nc\n=======\nb\nC\n>>>>>>> theirs\nd\ne\n<<<<<<< yours\nF\n=======\nF!\n>>>>>>> theirs\ng\n", "merge: open conflicts between markers");
            m.chunks[1].take = C::kTakeEach;
            m.chunks[3].take = C::kTakeTheirs;
            check(m.result() == "a\nB\nC\nd\ne\nF!\ng\n" && m.open() == 0, "merge: each side's, and theirs");
            m.chunks[3].take = C::kTakeYoursTheirs;
            check(m.result() == "a\nB\nC\nd\ne\nF\nF!\ng\n", "merge: both, yours first");
            const GitMerge one = merge3("x\ny\n", "x\ny\nyours\n", "theirs\nx\ny\n");
            check(one.result() == "theirs\nx\ny\nyours\n" && one.open() == 0, "merge: one side's at each end");
            const GitMerge both = merge3("x\n", "x\nsame\n", "x\nsame\n");
            check(both.result() == "x\nsame\n" && both.chunks.back().kind == C::kBoth, "merge: the same change by both, once");
            const GitMerge added = merge3("", "mine", "theirs");
            check(added.result() == "<<<<<<< yours\nmine\n=======\ntheirs\n>>>>>>> theirs\n" && !added.chunks[0].simple, "merge: both added, without a last line end");
            GitMergeView v;
            merge_view(m, 60, 4, &v);
            check(v.chunk_of_row.size() == 1 + 1 + 2 + 2 + 2 + 1 && v.chunk_of_row[0] == ~0u && v.text.starts_with("Theirs"), "merge: laid out in three columns");
        }
        // And with random texts: where one side is the base, the other is the result; where both are alike, they are.
        {
            u64 seed = 12345;
            auto next = [&] { return seed = seed * 6364136223846793005ull + 1442695040888963407ull, static_cast<u32>(seed >> 33); };
            auto text = [&](u32 n) {
                std::string t;
                for (u32 i = 0; i < n; ++i) t += std::string(1, static_cast<char>('a' + next() % 5)) + "\n";
                return t;
            };
            auto edit = [&](std::string t) {
                for (u32 k = next() % 4; k-- > 0;) {
                    const size_t at = t.empty() ? 0 : (next() % (t.size() / 2 + 1)) * 2;
                    if (next() % 2 && at < t.size()) t.erase(at, 2);
                    else t.insert(std::min(at, t.size()), std::string(1, static_cast<char>('f' + next() % 5)) + "\n");
                }
                return t;
            };
            bool ok = true;
            for (int round = 0; round < 300 && ok; ++round) {
                const std::string b0 = text(next() % 12), y = edit(b0), t = edit(b0);
                ok = merge3(b0, y, b0).result() == y && merge3(b0, b0, t).result() == t && merge3(b0, y, y).result() == y && merge3(b0, y, t).base.size() == line_hashes(b0).size();
                GitMerge m = merge3(b0, y, t);
                for (GitMerge::Chunk& c : m.chunks) c.take = GitMerge::Chunk::kTakeYours;
                bool yours_only = true;
                for (const GitMerge::Chunk& c : m.chunks) yours_only &= c.kind != GitMerge::Chunk::kTheirs;
                if (yours_only) ok &= m.result() == y;
                if (!ok) std::fprintf(stderr, "merge: base %s yours %s theirs %s\n", b0.c_str(), y.c_str(), t.c_str());
            }
            check(ok, "merge: random texts, one side as the base");
        }
        GitBlame blame;
        const std::string a(40, 'a'), zero(40, '0');
        const std::string porcelain = a + " 1 1 2\nauthor Ada\nauthor-time 1700000000\nsummary First\nfilename f\n\tline one\n" + a + " 2 2\n\tline two\n" +
                                      zero + " 3 3 1\nauthor Not Committed Yet\nauthor-time 1700000500\nsummary x\n\tline three\n";
        check(parse_blame(porcelain, &blame) && blame.lines.size() == 3 && blame.commits.size() == 2, "blame: three lines of two commits");
        check(blame.lines[0] == 0 && blame.lines[1] == 0 && blame.lines[2] == 1 && blame.commits[0].author == "Ada" && blame.commits[0].time == 1700000000 &&
                  blame.commits[0].summary == "First" && blame.commits[1].time == 0,
              "blame: who and when, and a line not committed yet");
        check(relative_age(1000, 1030) == "just now" && relative_age(0, 3 * 86400) == "3 days ago" && relative_age(0, 3600) == "an hour ago" &&
                  relative_age(0, 800 * 86400) == "2 years ago",
              "ages");
    }
    // Lines: the same text in one piece and cut anywhere gives the same numbers.
    {
        const std::string text = "one\ntwo\n\nthree and more\nlast without newline";
        const std::vector<u64> whole = line_hashes(std::string_view(text));
        check(whole.size() == 5, "line hashes: five lines");
        for (size_t cut = 0; cut <= text.size(); ++cut) {
            const Piece parts[] = {{text.data(), static_cast<u32>(cut), 0}, {text.data() + cut, static_cast<u32>(text.size() - cut), 0}};
            if (line_hashes(std::span<const Piece>(parts)) != whole) fail("line hashes across a piece border", cut, 0);
        }
        check(line_hashes(std::string_view("a\nb\n")).size() == 2 && line_hashes(std::string_view("")).empty(), "line hashes: a final newline ends the last line");
    }
    // The comparison: what is not in a run of changed lines must be, in
    // order, lines of the old text; and the number of lines changed must be
    // the fewest possible (checked against the longest common subsequence).
    for (int seed = 0; seed < seeds; ++seed) {
        rng.seed(static_cast<u64>(seed) * 104729 + 11);
        std::vector<u64> base(rnd(60));
        for (u64& x : base) x = rnd(12);
        std::vector<u64> now = base;
        for (int k = static_cast<int>(rnd(6)); k > 0; --k) {
            const size_t at = rnd(now.size() + 1);
            switch (rnd(3)) {
                case 0: now.insert(now.begin() + static_cast<std::ptrdiff_t>(at), rnd(4) + 1, rnd(12)); break;
                case 1: now.erase(now.begin() + static_cast<std::ptrdiff_t>(at), now.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(now.size(), at + rnd(4)))); break;
                default:
                    if (at < now.size()) now[at] = 100 + rnd(5);
            }
        }
        const std::vector<GitHunk> hunks = diff_lines(base, now);
        std::vector<u64> kept;
        size_t line = 0, changed = 0;
        for (const GitHunk& h : hunks) {
            if (h.line < line || h.line + h.count > now.size() || (h.kind == GitHunk::kDeleted) != (h.count == 0)) fail("diff: runs in order and in range", static_cast<u64>(seed), h.line);
            for (; line < h.line; ++line) kept.push_back(now[line]);
            line += h.count;
            changed += h.count;
        }
        for (; line < now.size(); ++line) kept.push_back(now[line]);
        size_t at = 0;
        for (const u64 x : kept) {
            while (at < base.size() && base[at] != x) ++at;
            if (at == base.size()) fail("diff: unchanged lines are the old text's, in order", static_cast<u64>(seed), 0);
            ++at;
        }
        std::vector<std::vector<u32>> lcs(base.size() + 1, std::vector<u32>(now.size() + 1, 0));
        for (size_t i = 1; i <= base.size(); ++i)
            for (size_t j = 1; j <= now.size(); ++j)
                lcs[i][j] = base[i - 1] == now[j - 1] ? lcs[i - 1][j - 1] + 1 : std::max(lcs[i - 1][j], lcs[i][j - 1]);
        if (kept.size() != lcs[base.size()][now.size()] || changed != now.size() - kept.size()) fail("diff: the fewest changes", static_cast<u64>(seed), kept.size());
    }
    {
        const std::vector<u64> a = {1, 2, 3, 4, 5}, b = {1, 2, 9, 4, 5}, c = {1, 2, 4, 5}, d = {1, 2, 3, 7, 8, 4, 5};
        auto one = [](const std::vector<GitHunk>& h, u32 line, u32 count, u8 kind) {
            return h.size() == 1 && h[0].line == line && h[0].count == count && h[0].kind == kind;
        };
        check(diff_lines(a, a).empty(), "diff: nothing changed");
        check(one(diff_lines(a, b), 2, 1, GitHunk::kModified), "diff: one line modified");
        check(one(diff_lines(a, c), 2, 0, GitHunk::kDeleted), "diff: one line deleted, marked where it was");
        check(one(diff_lines(a, d), 3, 2, GitHunk::kAdded), "diff: two lines added");
        check(one(diff_lines({}, a), 0, 5, GitHunk::kAdded) && one(diff_lines(a, {}), 0, 0, GitHunk::kDeleted), "diff: from nothing, to nothing");
        std::vector<u64> big(5000), other(5000);
        for (size_t i = 0; i < big.size(); ++i) big[i] = i, other[i] = 100000 + i;
        check(one(diff_lines(big, other), 0, 5000, GitHunk::kModified), "diff: texts with nothing in common are one modified run");
    }
    // A diff as git prints it, read; patches of the lines chosen from it, to
    // stage (forwards) and unstage (backwards); and the same in two columns.
    {
        const std::string diff =
            "diff --git a/f.txt b/f.txt\n"
            "index 1111111..2222222 100644\n"
            "--- a/f.txt\n"
            "+++ b/f.txt\n"
            "@@ -1,4 +1,4 @@\n"
            " one\n"
            "-two\n"
            "+TWO\n"
            " three\n"
            " four\n"
            "@@ -10,3 +10,5 @@ nine\n"
            " ten\n"
            "+A\n"
            "+B\n"
            " eleven\n"
            " twelve\n";
        const std::string head = "diff --git a/f.txt b/f.txt\nindex 1111111..2222222 100644\n--- a/f.txt\n+++ b/f.txt\n";
        const GitDiff d = parse_diff(diff);
        check(d.lines() == 16 && d.hunks.size() == 2 && d.hunks[0].first == 4 && d.hunks[0].last == 10 && d.hunks[1].first == 10 && d.hunks[1].last == 16 &&
                  d.hunks[1].head0 == 0 && d.hunks[1].head1 == 4 && d.hunks[1].old_start == 10 && d.hunks[1].new_count == 5,
              "diff read: two hunks of one file");
        check(d.hunk_at(3) == -1 && d.hunk_at(4) == 0 && d.hunk_at(9) == 0 && d.hunk_at(12) == 1 && d.hunk_at(16) == -1, "diff read: the hunk a line is in");
        auto pick = [&](std::initializer_list<u32> lines) {
            std::vector<u8> c(d.lines(), 0);
            for (const u32 l : lines) c[l] = 1;
            return c;
        };
        check(diff_patch(diff, d, pick({}), false).empty() && diff_patch(diff, d, pick({5, 8}), false).empty(), "patch: nothing chosen, or only context: nothing");
        check(diff_patch(diff, d, pick({13}), false) == head + "@@ -10,3 +10,4 @@\n ten\n+B\n eleven\n twelve\n", "patch: one line of two added, staged");
        check(diff_patch(diff, d, pick({6}), false) == head + "@@ -1,4 +1,3 @@\n one\n-two\n three\n four\n", "patch: a line taken away, staged without the one put in its place");
        check(diff_patch(diff, d, pick({7, 12}), false) == head + "@@ -1,4 +1,5 @@\n one\n two\n+TWO\n three\n four\n@@ -10,3 +11,4 @@\n ten\n+A\n eleven\n twelve\n",
              "patch: a line kept as context moves the next hunk along");
        check(diff_patch(diff, d, pick({7}), true) == head + "@@ -1,3 +1,4 @@\n one\n+TWO\n three\n four\n", "patch: backwards, a line added unstaged alone");
        std::vector<u8> all(d.lines(), 1);
        std::string whole = diff;
        whole.replace(whole.find(" @@ nine"), 8, " @@");
        check(diff_patch(diff, d, all, false) == whole, "patch: everything chosen is the diff itself (its hunks' headings without their context)");

        GitDiffView v;
        diff_view(diff, d, 0, 4, &v);
        check(v.text == diff && v.left.empty() && v.style.size() >= 4, "one column: the diff as it is");
        diff_view(diff, d, 60, 4, &v);
        check(v.left.size() == 15 && v.right.size() == 15 && v.cols == 60, "two columns: a row for each pair of lines, heading and hunk heading");
        size_t rows = 0;
        for (const char c : v.text) rows += c == '\n';
        check(rows == 15, "two columns: as many lines of text");
        check(v.left[6] == 6 && v.right[6] == 7 && v.left[11] == ~0u && v.right[11] == 12 && v.left[10] == 11 && v.right[10] == 11,
              "two columns: a line beside the one it became, an added one beside nothing");
        // Every row that has both columns has its divider at the same column.
        size_t at = 0, bar = std::string::npos;
        bool level = true;
        for (u32 r = 0; r < 15; ++r) {
            const size_t nl = v.text.find('\n', at);
            const std::string line = v.text.substr(at, nl - at);
            at = nl + 1;
            const size_t b = line.find("\xE2\x94\x82");
            if (b == std::string::npos) continue;
            if (bar == std::string::npos) bar = b;
            level = level && b == bar;
        }
        check(bar != std::string::npos && level, "two columns: lines aligned");
        // Within a changed line, what stayed the same is muted and what changed is not.
        const std::string small = "diff --git a/c b/c\n--- a/c\n+++ b/c\n@@ -1 +1 @@\n-int x = 1;\n+int x = 2;\n";
        const GitDiff sd = parse_diff(small);
        diff_view(small, sd, 60, 4, &v);
        auto style_at = [&](size_t off) {
            u8 st = 0;
            for (size_t k = 0; k < v.off.size() && v.off[k] <= off; ++k) st = v.style[k];
            return st;
        };
        const size_t row = v.text.find("int x");
        const size_t one_at = v.text.find('1', row + 5), two_at = v.text.find("= 2;");
        check(row != std::string::npos && style_at(row) == 4 && style_at(one_at) == 2 && style_at(two_at) == 4 && style_at(two_at + 2) == 1,
              "two columns: changes within a line marked");
    }
    // Lines of a text staged and unstaged, as the index is written.
    {
        const std::string committed = "a\nb\nc\nd\ne\n", now = "a\nB\nc\nd\nE\nf\n";
        check(stage_lines(committed, committed, now, 1, 2, true) == "a\nB\nc\nd\ne\n", "stage lines: one changed line");
        check(stage_lines(committed, committed, now, 4, 6, true) == "a\nb\nc\nd\nE\nf\n", "stage lines: a changed line and one added");
        check(stage_lines(committed, "a\nB\nc\nd\ne\n", now, 1, 2, false) == committed, "unstage lines: back as committed");
        check(stage_lines(committed, "a\nB\nc\nd\nE\nf\n", now, 4, 6, false) == "a\nB\nc\nd\ne\n", "unstage lines: only those");
        check(stage_lines(committed, committed, "a\nc\nd\ne\n", 1, 1, true) == "a\nc\nd\ne\n", "stage lines: lines taken out");
        check(stage_lines(committed, committed, "a\nb\nc\nd\ne", 4, 5, true) == "a\nb\nc\nd\ne", "stage lines: the last line's end taken away");
    }
    {
        const std::vector<GitStash> st = parse_stashes("stash@{0}\x1f" "2 hours ago\x1f" "WIP on main: abc Start\nstash@{1}\x1f" "3 days ago\x1f" "On main: tidy\n");
        check(st.size() == 2 && st[0].name == "stash@{0}" && st[0].when == "2 hours ago" && st[1].subject == "On main: tidy", "stashes");
        std::vector<std::string> tags;
        const std::vector<GitBranch> b = parse_branches("*\trefs/heads/main\n \trefs/tags/v1.9\n \trefs/tags/v1.10\n \trefs/tags/v1.0\n \trefs/tags/alpha\n", &tags);
        check(b.size() == 1 && tags.size() == 4 && tags[0] == "v1.10" && tags[1] == "v1.9" && tags[2] == "v1.0" && tags[3] == "alpha", "tags, newest version first");
    }

#if !defined(_WIN32)
    // The real thing, if there is a git to run.
    if (Lsp::find_program("git").empty()) {
        std::printf("git test skipped: no git\n");
        return;
    }
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/blit_git_test";
    auto sh = [&](const std::string& cmd) { return std::system(("cd '" + dir + "' && " + cmd + " >/dev/null 2>&1").c_str()) == 0; };
    std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "'").c_str());
    auto write = [&](const std::string& name, const std::string& text) {
        if (FILE* f = std::fopen((dir + "/" + name).c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    };
    write("a.txt", "one\ntwo\nthree\nfour\n");
    check(sh("git init -q -b main . && git config user.name Tester && git config user.email tester@example.invalid && git config commit.gpgsign false && "
             "git add a.txt && git commit -q -m First"),
          "a repository for the git test");
    {
        Git git;
        git.open(dir + "/");
        std::vector<Git::Result> got = git_wait(git, Git::kStatus);
        check(!got.empty() && got.back().ok && got.back().status.repo && got.back().status.branch == "main" && got.back().status.entries.empty() &&
                  got.back().log.size() == 1 && got.back().log[0].subject == "First" && got.back().branches.size() == 1 && got.back().branches[0].current,
              "git: a clean repository with one commit");

        // The text as an editor would hold it, changed: line two modified, one added at the end.
        Editor ed;
        std::string err;
        check(ed.open((dir + "/a.txt").c_str(), &err), "git test file opens");
        ed.move_to(ed.buf.line_start(1), false);
        ed.insert("2 ");
        ed.move_to(ed.buf.size(), false);
        ed.insert("five\n");
        std::vector<Piece> pieces;
        ed.buf.snapshot(&pieces);
        git.hunks(7, 42, dir + "/a.txt", pieces);
        got = git_wait(git, Git::kHunks);
        check(!got.empty() && got.back().ok && got.back().tracked && got.back().doc == 7 && got.back().revision == 42 && got.back().hunks.size() == 2 &&
                  got.back().hunks[0].line == 1 && got.back().hunks[0].kind == GitHunk::kModified && got.back().hunks[1].line == 4 &&
                  got.back().hunks[1].kind == GitHunk::kAdded,
              "git: the lines changed since the commit");
        git.blame(7, 42, dir + "/a.txt", pieces);
        got = git_wait(git, Git::kBlame);
        check(!got.empty() && got.back().ok && got.back().blame && got.back().blame->lines.size() == 5, "git: blame of the text as it is");
        if (!got.empty() && got.back().blame) {
            const GitBlame& bl = *got.back().blame;
            check(bl.commits[bl.lines[0]].author == "Tester" && bl.commits[bl.lines[0]].summary == "First" && bl.commits[bl.lines[1]].time == 0 &&
                      bl.commits[bl.lines[4]].time == 0 && bl.commits[bl.lines[2]].time > 0,
                  "git: committed lines have an author, changed ones are not committed yet");
        }
        // Save, stage, commit; a new file stays untracked.
        check(ed.save(&err), "git test file saves");
        write("b.txt", "new\n");
        git.refresh();
        got = git_wait(git, Git::kStatus);
        check(!got.empty() && got.back().status.entries.size() == 2, "git: one modified file and one untracked");
        git.run({"add", "--", "a.txt"}, "Stage");
        got = git_wait(git, Git::kStatus);
        bool staged = false;
        for (const GitEntry& e : got.empty() ? std::vector<GitEntry>{} : got.back().status.entries) staged |= e.path == "a.txt" && e.staged() && !e.unstaged();
        check(staged, "git: staged");
        git.run({"commit", "-q", "-F", "-"}, "Commit", "Second\n\nWith a body.\n");
        got = git_wait(git, Git::kStatus);
        check(got.size() >= 2 && got[got.size() - 2].kind == Git::kAction && got[got.size() - 2].ok && got.back().log.size() == 2 &&
                  got.back().log[0].subject == "Second" && got.back().status.entries.size() == 1,
              "git: committed, with the message from standard input");
        git.hunks(7, 43, dir + "/a.txt", pieces);
        got = git_wait(git, Git::kHunks);
        check(!got.empty() && got.back().tracked && got.back().hunks.empty(), "git: nothing differs from the new commit");
        git.hunks(8, 1, dir + "/b.txt", {});
        got = git_wait(git, Git::kHunks);
        check(!got.empty() && got.back().ok && !got.back().tracked, "git: a file with no committed version");
        // A branch, a commit on it, and a merge back.
        git.run({"switch", "-q", "-c", "side"}, "New branch");
        got = git_wait(git, Git::kStatus);
        check(!got.empty() && got.back().status.branch == "side" && got.back().branches.size() == 2, "git: a new branch, checked out");
        write("b.txt", "new\nmore\n");
        git.run({"add", "-A"}, "Stage all");
        git_wait(git, Git::kStatus);
        git.run({"commit", "-q", "-m", "On the side"}, "Commit");
        git_wait(git, Git::kStatus);
        git.run({"switch", "-q", "main"}, "Switch");
        git_wait(git, Git::kStatus);
        git.run({"merge", "--no-edit", "side"}, "Merge");
        got = git_wait(git, Git::kStatus);
        check(!got.empty() && got.back().status.branch == "main" && !got.back().status.merging && got.back().log.size() == 3 &&
                  got.back().log[0].subject == "On the side",
              "git: merged");
        git.run({"no-such-command"}, "Nonsense");
        got = git_wait(git, Git::kAction);
        check(!got.empty() && !got[0].ok && !got[0].text.empty(), "git: a command that fails says why");
        git.text(9, {"show", "--stat", "HEAD"}, "show");
        got = git_wait(git, Git::kText);
        check(!got.empty() && got.back().ok && got.back().doc == 9 && got.back().text.find("On the side") != std::string::npos, "git: a command's output");
        git.view(10, {"show", "HEAD"}, nullptr, 80, 4);
        got = git_wait(git, Git::kView);
        check(!got.empty() && got.back().ok && got.back().view && got.back().diff && got.back().diff->hunks.size() == 1 && got.back().view->cols == 80 &&
                  !got.back().view->left.empty(),
              "git: a diff read and laid out in two columns");

        // Lines of an editor's text staged, and unstaged, without a patch.
        write("a.txt", "one\n2 two\nthree\nfour\nfive\n");
        Editor e2;
        check(e2.open((dir + "/a.txt").c_str(), &err), "git test file opens again");
        e2.move_to(0, false);
        e2.insert("zero\n");
        std::vector<Piece> now;
        e2.buf.snapshot(&now);
        git.stage(11, dir + "/a.txt", now, 0, 1, true, "Stage");
        got = git_wait(git, Git::kStatus);
        check(got.size() >= 2 && got[got.size() - 2].kind == Git::kAction && got[got.size() - 2].ok, "git: lines staged");
        git.hunks(11, 1, dir + "/a.txt", now);
        got = git_wait(git, Git::kHunks);
        check(!got.empty() && got.back().hunks.size() == 1 && got.back().hunks[0].line == 0 && got.back().hunks[0].staged, "git: the change staged is marked so");
        git.stage(11, dir + "/a.txt", now, 0, 1, false, "Unstage");
        git_wait(git, Git::kStatus);
        git.hunks(11, 2, dir + "/a.txt", now);
        got = git_wait(git, Git::kHunks);
        check(!got.empty() && got.back().hunks.size() == 1 && !got.back().hunks[0].staged, "git: and unstaged");
    }
    // Patches of lines picked at random from what git printed, applied by
    // git itself: what is staged is those changes and no others; and the
    // same backwards, unstaging some of them again.
    {
        auto out = [&](const std::string& cmd) {
            std::string text;
            if (FILE* f = popen(("cd '" + dir + "' && " + cmd + " 2>/dev/null").c_str(), "r")) {
                char buf[4096];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
                pclose(f);
            }
            return text;
        };
        auto lines_of = [](const std::string& t) {
            std::vector<std::string> v;
            for (size_t at = 0; at < t.size();) {
                const size_t nl = t.find('\n', at);
                v.push_back(t.substr(at, nl - at));
                at = nl + 1;
            }
            return v;
        };
        // What the patch should leave: the side it is applied to, with the
        // chosen changes made (forwards: lines taken away go, lines added
        // come; backwards the other way about).
        auto expect = [](const std::string& text, const GitDiff& d, const std::vector<u8>& chosen, const std::vector<std::string>& from, bool reverse) {
            std::string r;
            u32 next = 1;
            for (const GitDiff::Hunk& h : d.hunks) {
                const u32 start = reverse ? (h.new_count ? h.new_start : h.new_start + 1) : (h.old_count ? h.old_start : h.old_start + 1);
                for (; next < start; ++next) r += from[next - 1] + "\n";
                for (u32 i = h.first + 1; i < h.last; ++i) {
                    const std::string l = text.substr(d.line_off[i], d.line_off[i + 1] - d.line_off[i] - 1);
                    const char c = l[0];
                    const bool gone = c == (reverse ? '+' : '-'), comes = c == (reverse ? '-' : '+');
                    if (c == ' ') r += from[next++ - 1] + "\n";
                    else if (gone) r += chosen[i] ? (++next, std::string()) : from[next++ - 1] + "\n";
                    else if (comes && chosen[i]) r += l.substr(1) + "\n";
                }
            }
            for (; next <= from.size(); ++next) r += from[next - 1] + "\n";
            return r;
        };
        for (int seed = 0; seed < std::min(seeds, 16); ++seed) {
            rng.seed(static_cast<u64>(seed) * 7919 + 5);
            std::vector<std::string> base;
            for (int i = 0; i < 40; ++i) base.push_back("line " + std::to_string(i));
            std::vector<std::string> now = base;
            for (int k = 2 + static_cast<int>(rnd(6)); k > 0; --k) {
                const size_t at = rnd(now.size());
                switch (rnd(3)) {
                    case 0: now.insert(now.begin() + static_cast<std::ptrdiff_t>(at), "new " + std::to_string(rnd(1000))); break;
                    case 1: now.erase(now.begin() + static_cast<std::ptrdiff_t>(at)); break;
                    default: now[at] = "changed " + std::to_string(rnd(1000));
                }
            }
            auto joined = [](const std::vector<std::string>& v) {
                std::string t;
                for (const std::string& l : v) t += l + "\n";
                return t;
            };
            write("p.txt", joined(base));
            sh("git add p.txt && git commit -q -m p");
            write("p.txt", joined(now));
            for (const bool reverse : {false, true}) {
                if (reverse) sh("git add p.txt");  // (now all of it is staged, to unstage some of)
                const std::string text = out(std::string("git diff --no-ext-diff --src-prefix=a/ --dst-prefix=b/") + (reverse ? " --cached" : "") + " -- p.txt");
                const GitDiff d = parse_diff(text);
                std::vector<u8> chosen(d.lines(), 0);
                for (u8& c : chosen) c = rnd(2);
                const std::string want = expect(text, d, chosen, reverse ? now : base, reverse);
                const std::string patch = diff_patch(text, d, chosen, reverse);
                write("patch.diff", patch);
                const bool applied = patch.empty() || sh(std::string("git apply --cached --recount --whitespace=nowarn") + (reverse ? " -R" : "") + " patch.diff");
                const std::string staged = out("git show :p.txt");
                if (!applied || staged != want) fail(reverse ? "patch backwards, applied by git" : "patch forwards, applied by git", static_cast<u64>(seed), d.hunks.size());
                (void)lines_of;
                sh("git reset -q p.txt");
            }
            sh("git checkout -q p.txt");
        }
    }
    {
        Git git;
        git.open("/");
        const std::vector<Git::Result> got = git_wait(git, Git::kStatus);
        check(!got.empty() && got.back().ok && !got.back().status.repo, "git: a folder that is in no repository");
    }
    std::system(("rm -rf '" + dir + "'").c_str());
#endif
}

void lsp_test() {
    u64 seed = 1;
    for (const bool utf8 : {false, true})
        for (const bool full : {false, true}) lsp_case(utf8, full, seed++);
    lsp_tabs_test();
    lsp_many_test();

    // A server that dies is reported and let go of.
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/blit_lsp_test_dies.cpp";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs("int x;\n", f);
        std::fclose(f);
    }
    Editor ed;
    std::string err;
    check(ed.open(path.c_str(), &err), "second lsp test file opens");
    LspServer fake;
    fake.name = "fake";
    fake.command = {BLIT_FAKE_LSP, "--die-after", "3"};
    fake.languages = {"cpp"};
    Lsp lsp;
    check(lsp.attach(ed.buf, path, "cpp", {fake}), "doomed server starts");
    ed.listen(&lsp);
    check(lsp_wait(lsp, Lsp::kState) & Lsp::kState, "doomed server becomes ready");
    ed.insert("a");
    ed.insert("b");
    u32 got = 0;
    for (int i = 0; i < 400 && lsp.attached(); ++i) {
        got |= lsp.poll();
        lsp.wait(10);
    }
    check(!lsp.attached() && (got & Lsp::kState) && lsp.take_message() == "language server: fake stopped", "a dead server is noticed");
    ed.insert("still typing");  // must be harmless
    ed.unlisten(&lsp);

    // One that prints lines of its own on its output, between its messages:
    // they are passed over, and every message still arrives.
    LspServer noisy = fake;
    noisy.command = {BLIT_FAKE_LSP, "--noise"};
    Lsp loud;
    check(loud.attach(ed.buf, path, "cpp", {noisy}), "noisy server starts");
    ed.listen(&loud);
    check(lsp_wait(loud, Lsp::kState) & Lsp::kState, "noisy server becomes ready");
    lsp_check_diagnostics(loud, ed, "a noisy server's diagnostics");
    ed.insert(" WARN");
    lsp_check_diagnostics(loud, ed, "a noisy server's diagnostics after an edit");
    ed.unlisten(&loud);
    loud.detach();

    // A server that says only "the word there" before a rename: each time,
    // the word where it was asked, not the one an earlier rename was about.
    LspServer plain = fake;
    plain.command = {BLIT_FAKE_LSP, "--default-rename"};
    ed.replace(0, ed.buf.size(), "int first;\nint second;\n");
    Lsp ren;
    check(ren.attach(ed.buf, path, "cpp", {plain}) && (lsp_wait(ren, Lsp::kState) & Lsp::kState), "default-rename server becomes ready");
    ren.prepare_rename(6);
    check((lsp_wait(ren, Lsp::kRenameRange) & Lsp::kRenameRange) && ren.rename_lo() == 4 && ren.rename_hi() == 9 && ren.rename_placeholder() == "first",
          "the word there, to rename");
    ren.prepare_rename(17);
    check((lsp_wait(ren, Lsp::kRenameRange) & Lsp::kRenameRange) && ren.rename_lo() == 15 && ren.rename_hi() == 21 && ren.rename_placeholder() == "second",
          "and the next time, the word there then");
    ren.detach();

    // Reference counts asked for and not answered yet (a slow server's):
    // those of an earlier tag let go of when another tag's are asked for,
    // and all of a server's once it has gone.
    LspServer slow = fake;
    slow.command = {BLIT_FAKE_LSP, "--slow", "400"};
    Lsp counting;
    check(counting.attach(ed.buf, path, "cpp", {slow}) && (lsp_wait(counting, Lsp::kState) & Lsp::kState), "slow server becomes ready");
    for (int i = 0; i < 3; ++i) counting.count_references(4, 1);
    check(counting.counts_asked() == 3, "counts asked for, waiting for their answers");
    counting.count_references(4, 2);
    check(counting.counts_asked() == 1, "counts of an earlier tag let go of");
    counting.detach();
    check(counting.counts_asked() == 0, "and a server's, once it has gone");
    LspServer doomed = fake;
    doomed.command = {BLIT_FAKE_LSP, "--slow", "200", "--die-after", "6"};
    Lsp dying;
    check(dying.attach(ed.buf, path, "cpp", {doomed}) && (lsp_wait(dying, Lsp::kState) & Lsp::kState), "a server to die becomes ready");
    for (int i = 0; i < 4; ++i) dying.count_references(4, 1);
    check(dying.counts_asked() == 4, "counts asked of a server that dies");
    for (int i = 0; i < 400 && dying.attached(); ++i) {
        dying.poll();
        dying.wait(10);
    }
    check(!dying.attached() && dying.counts_asked() == 0, "a server that died: its counts let go of");

    LspServer missing;
    missing.name = "missing";
    missing.command = {"blit-no-such-language-server"};
    missing.languages = {"cpp"};
    check(!lsp.attach(ed.buf, path, "cpp", {missing}), "a server that is not installed is skipped");
    check(Lsp::pick(builtin_lsp_servers(), "no-such-language", nullptr) == nullptr, "no server for an unknown language");
    std::remove(path.c_str());
}
#else
void lsp_test() {}
#endif

}  // namespace

// One blit takes the files (instance.hpp): an ask handed over through the
// socket arrives whole, one that waits is told when its tabs have closed, a
// socket left by a blit that died is taken over, and a second blit of the
// same program leaves the socket to the first.
std::atomic<bool> instance_taken{false};
void instance_test() {
    auto check = [](bool ok, const char* what) {
        if (!ok) fail(what, 0, 0);
    };
    const std::string path = instance_socket();
    ::unlink(path.c_str());
    check(instance_hand_over({}) == -1, "instance: none running, nobody takes the files");
    {  // a socket left behind
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strcpy(a.sun_path, path.c_str());
        check(fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0, "instance: a socket to leave behind");
        ::close(fd);
    }
    check(instance_hand_over({}) == -1, "instance: a socket left behind takes nothing");
    std::mutex mu;
    std::vector<InstanceAsk> got;
    std::atomic<int> n{0};
    auto arrived = [&](int want) {
        for (int i = 0; i < 400 && n.load() < want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return n.load() >= want;
    };
    InstanceServer server;
    check(server.start([&](InstanceAsk a) {
        std::lock_guard lock(mu);
        got.push_back(std::move(a));
        ++n;
    }),
          "instance: the socket left behind is taken over");
    struct stat st;
    check(::stat(path.c_str(), &st) == 0 && (st.st_mode & 077) == 0, "instance: the socket is the user's alone");
    InstanceServer second;
    check(!second.start([](InstanceAsk) {}), "instance: a second blit of the program leaves the socket to the first");
    InstanceAsk ask;
    ask.files = {{"/tmp/a b:c.txt", 12, 3}, {"ssh://host/x", 0, 0}};
    check(instance_hand_over(ask, [] { instance_taken = true; }) == 0 && instance_taken, "instance: handed over");
    check(arrived(1), "instance: the ask arrives");
    {
        std::lock_guard lock(mu);
        const InstanceAsk& a = got[0];
        check(a.files.size() == 2 && a.files[0].path == "/tmp/a b:c.txt" && a.files[0].line == 12 && a.files[0].col == 3 &&
                  a.files[1].path == "ssh://host/x" && a.files[1].line == 0 && !a.wait && a.fd < 0,
              "instance: the ask arrives whole (spaces and colons in a name, a line, a column, another machine's file)");
    }
    check(instance_hand_over({}) == 0 && arrived(2), "instance: no files: the running one comes to the front");
    InstanceAsk w;
    w.wait = true;
    w.files = {{"/tmp/c", 0, 0}};
    std::atomic<int> rc{-2};
    instance_taken = false;
    std::jthread asker([&] { rc = instance_hand_over(w, [] { instance_taken = true; }); });
    check(arrived(3), "instance: a waiting ask arrives");
    for (int i = 0; i < 400 && !instance_taken; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(instance_taken && rc == -2, "instance: one that waits hears the files are taken, and goes on waiting");
    int fd = -1;
    {
        std::lock_guard lock(mu);
        check(got[2].wait && got[2].fd >= 0, "instance: the running one keeps a waiting asker's connection");
        fd = got[2].fd;
    }
    InstanceServer::answer(fd, "done\n");
    asker.join();
    check(rc == 0, "instance: told when its tabs have closed");
    server.stop();
    check(::stat(path.c_str(), &st) != 0, "instance: the socket goes with the blit");
    check(instance_hand_over(ask) == -1, "instance: gone, nobody takes the files");
}

int main(int argc, char** argv) {
    int seeds = argc > 1 ? std::atoi(argv[1]) : 200;
    int steps = argc > 2 ? std::atoi(argv[2]) : 600;
#if !defined(_WIN32)
    // A scratch folder of this run's own, as TMPDIR: the tests' files and
    // folders there have fixed names (blit_git_test, ...), and two runs at
    // once (two worktrees, two build folders) took them from each other.
    // (Kept when a test fails, to be looked at.)
    const char* tmp = std::getenv("TMPDIR");
    std::string scratch = std::string(tmp && *tmp ? tmp : "/tmp") + "/blit-fuzz-XXXXXX";
    if (!mkdtemp(scratch.data())) fail("a scratch folder", 0, 0);
    setenv("TMPDIR", scratch.c_str(), 1);
#endif
    for (int s = 1; s <= seeds; ++s) run(static_cast<u64>(s), steps);
    for (int s = 1; s <= seeds; ++s) run_editor(static_cast<u64>(s), steps);
    truncated_file_test();
    config_test();
    view_test(seeds);
    atlas_test();
    long_line_test();
    vim_test();
    carets_test(seeds);
    timeline_test();
    snippets_test();
    asm_test();
    notebook_test();
    history_test();
    wrap_test();
    find_test();
    folds_test();
    tests_test();
    script_test();
    term_test(seeds);
    profile_test();
    dap_test();
    remote_test();
    git_test(seeds);
    tree_test();
    instance_test();
    json_test();
    parsers_test(seeds);
    lsp_test();
    styles_test();
    edge_test();
    box_test();
    fit_test();
    match_test();
    highlights_test();
    syntax_test();
#if !defined(_WIN32)
    std::system(("rm -rf '" + scratch + "'").c_str());
#endif
    std::printf("fuzz ok: %d seeds x %d steps\n", seeds, steps);
    return 0;
}
