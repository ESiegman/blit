# Benchmarks

Every number here was measured on one machine: Apple M5 Pro, macOS 27.2,
SDL 3.4.20 on its Metal backend, GCC 16.2 (`-O3 -mcpu=native -flto`), a
4608×2592 display at 160 Hz. Raw results for each round are in
`bench/results/*.jsonl`.

## Running them

The benchmark tools and the editor's `--bench` mode are compiled only in a
Debug build (a Release build is the editor alone). A plain Debug build is
unoptimized, so the suite runs from a Debug build that is compiled exactly
like a release, kept in `build-bench/`; `bench/run.sh` refuses any other.

```bash
cmake -S . -B build-bench -G Ninja -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_BUILD_TYPE=Debug -DBLIT_OPTIMIZE_DEBUG=ON
cmake --build build-bench
bench/run.sh my-label          # everything; opens a window for a few seconds
bench/run.sh my-label --no-window
bench/report.py bench/results/r0-baseline.jsonl bench/results/my-label.jsonl
```

(Rounds 0 to 5 were measured when a Release build still carried the tools.
The flags are the same, so the numbers compare.)

The suite turns language servers off (a real one working in the background
would be measured along with everything else) and measures the language
server client separately, against the test server `blit_fakelsp`.

`bench/run.sh` generates the test files on first use (about 1.2 GB under
`bench/data`): 1 MB and 100 MB of C-like code, a 1 GB log, one 64 MB line,
and 16 MB of mixed-script UTF-8.

There are two kinds of benchmark:

- `blit_bench` measures the text core with no window and no GPU.
- `blit --bench NAME` runs inside the real app on the real GPU path, either
  into an offscreen texture (`--offscreen WxH`) or a real window.

`bench/frame.py SCENARIO FILE [args]` prints one scenario as a single line,
which is handy while changing something.

Settings for the benchmarks are `init.lua` files (`BLIT_INIT` names one);
`bench/run.sh` writes them into `bench/data`, among them `everything.lua`,
with which `scroll_everything` and `keys_everything` run as blit is used:
a C++ file in colour, the test language server attached, the looks.

`blit_bench tree DIR [words]` times the finder on a real folder: reading
it, finding names in it, and searching its files for each word given.

`bench/profile.sh SCENARIO FILE [args]` runs a scenario for a few seconds
under macOS's `sample` and prints the main thread's time by function. Timing
says whether something got slower; this says where the time is. (`leaks
--atExit -- ./build-bench/blit ...` and `vmmap --summary PID` are the other
two tools used below. `xctrace` needs the full Xcode, which this machine does
not have.)

## Where it stands

Baseline is the first working version. Final is the current code.

### Text core (100 MB code file unless noted)

| What | Baseline | Final | |
|---|---:|---:|---:|
| Open 100 MB | 3.35 ms | 1.99 ms | 1.7x |
| Open 1 GB | 26.3 ms | 17.7 ms | 1.5x |
| Scan all text for newlines | 4.3 GB/s | 70 GB/s | 16x |
| Find the start of a random line | 581 ns | 266 ns | 2.2x |
| Find the line of a random offset | 574 ns | 200 ns | 2.9x |
| Type one character | 21.8 ns | 21.6 ns | same |
| Backspace (long run) | 31,600 ns | 90 ns | 350x |
| Insert at a random offset | 348 ns | 257 ns | 1.4x |
| Undo/redo one step | 73 ns | 55 ns | 1.3x |
| Paste 8 MB | 3.4 GB/s | 12.3 GB/s | 3.6x |
| Text to cell grid, one frame (290×82 cells) | 1.72 µs | 1.18 µs | 1.5x |
| Same, mixed-script UTF-8 | 8.9 µs | 6.1 µs | 1.5x |
| Same, scrolled 60 M columns into one line | 1,295 µs | 1.8 µs | 730x |
| Tree memory for a 1 GB file | | 4.3 MB | |

### Frames

| What | Baseline | Final | |
|---|---:|---:|---:|
| Smooth scroll in a window, frames late out of ~635 | 73 | 0 to 8 | |
| Smooth scroll, worst gap between frames | 36.8 ms | 13.6 ms | 2.7x |
| GPU throughput at 4608×2592 (frames/s, GPU bound) | 3,730 | 7,600 | 2.0x |
| GPU throughput at 2560×1600 (frames/s) | 9,160 | 17,800 to 20,700 | 2x |
| CPU per frame, back to back (offscreen) | 88 µs | 43 µs | 2.0x |
| CPU per frame while scrolling in a window | 106 µs | 96 µs | 1.1x |
| Idle for 3 s: frames drawn, CPU | 1, 0.26% | 0, 0.04% | |
| Keypress after a pause, event to frame submitted | | 247 µs (p99 472) | |

The 4608×2592 pair was measured back to back in one sitting. The 2560×1600
range is two full-suite runs taken at different times; see "Reading these
numbers" below.

### Finding (Round 17; there was nothing before it)

| What | |
|---|---:|
| Rank 200,000 paths against what is typed | 8.4 ms |
| Search text in memory for a word | 2.4 to 64 GB/s |
| Read a folder of 37,757 files | 0.095 s (Round 20; 0.18 to 0.22 s before) |
| Find a name in it | 2.2 to 3.9 ms |
| Search its 334 MB for a word | 0.27 to 0.28 s |

### Scripts (Round 18; there was nothing before it)

| What | |
|---|---:|
| A key, no init.lua (against before scripts) | the same |
| A key, with a usual init.lua | +0.005 to +0.025 µs |
| A handler run for an event | 0.4 to 0.6 µs |
| A mapped key calling a Lua function | 0.15 µs |
| Startup: Lua and a usual init.lua | 0.43 ms |

### Rendering (Round 19: Metal of blit's own, against SDL_GPU)

| What | SDL_GPU | Metal |
|---|---:|---:|
| CPU per frame, paced at 160 a second | 27.0 µs | 18.0 µs |
| A key press to its frame handed to the GPU, p50 (window) | 96 µs | 60 µs |
| Back to back, frames a second at 2560x1600 | 18,020 | 18,270 |
| Making the GPU device and pipelines | 6.7 ms | 5.0 to 5.5 ms |

### Vim on large files and latency (Rounds 20 and 21)

| What (1 GB log file) | Before | Now |
|---|---:|---:|
| `}` or `{` across the file | 1.4 s | 70 ms |
| `/` typed, nothing found | 232 ms | 8 ms |
| `/word\c` | 9.1 s | 23 ms |
| `:%s/word/x/`, nothing found | 1.85 s | 29 ms |
| A key typed while the view moves, to its frame (p50, 60 Hz) | 25 ms | 8.6 ms |
| A key after a pause: handed over to on the screen (p50, Round 21) | 39.5 ms | 15 ms |
| `:%s/INFO/WARN/` on the 1 GB log (Round 21) | 3.57 s, 5.3 GB | 0.41 s, 2.7 GB |

### Startup (time from `exec` to the first frame finished)

| What | Baseline | Final |
|---|---:|---:|
| blit, 1 MB file | 136.5 ms | 124.7 ms |
| blit, 1 GB file | 160.7 ms | 129.8 ms |
| An SDL program that opens a window and draws nothing | | 125.1 ms |

Startup is at the floor. Of the 125 ms, about 8 ms passes before `main`
(loading SDL and the system frameworks), 78 ms is `SDL_Init` (AppKit bringing
up the application) and 31 ms is creating the window. blit's own work after
the window exists is 9 ms, and that includes waiting for the first vsync.

## What was done, round by round

### Round 0: baseline

Piece tree over a memory-mapped file, one-draw-call cell-grid renderer,
straightforward scalar loops.

### Round 1: text core

- **Backspace was quadratic.** A run of backspaces folds into one undo step,
  and each one prepended to that step's list of removed pieces. Now the list
  is kept reversed during the run (so it is an append) and pieces that are
  neighbours in memory merge. 31.6 µs → 90 ns per backspace.
- **Newline scanning.** `std::count` counted into a 64-bit total, which the
  vectorizer handles badly (4.3 GB/s). Rewritten with GCC's portable vector
  types and byte-wide counters: 70 GB/s. This speeds up loading, pasting,
  line lookups and every piece split.
- **Finding the Nth newline** skips 64 bytes at a time instead of calling
  `memchr` per line.
- **ASCII run detection** in the grid fill checks 16 bytes at a time.

### Round 2: frames

- **Present mode.** With SDL's vsync mode on this Mac, a vsynced smooth
  scroll dropped frames: 16 to 70 late frames per 600 in an interleaved test,
  and up to 40% in the worst runs. "Immediate" mode in a window is still
  synchronized by the macOS compositor (no tearing), and the same test gave
  0 to 8 late frames. blit now uses immediate for windows on macOS and vsync
  everywhere else, and for macOS fullscreen. `--present` overrides it.
- **Shader: no integer divide.** Glyph slots per atlas row is now a power of
  two, so the fragment shader splits a slot number with a shift and a mask
  instead of `%` and `/`. 3,730 → 5,320 frames/s at 4608×2592.
- **Shader: blank cells skip the atlas.** → 5,710.
- **Shader: cursor and scroll thumb tests are gated.** The CPU sets one bit
  on the few cells those rectangles touch; all other pixels skip the tests.
  The bounds check became a single clamp. → 7,600.
- **Unchanged grids are not uploaded.** A frame whose cells match what the
  GPU already has (cursor moved within a cell, window exposed) skips the
  copy pass.
- **Non-ASCII glyph lookup** is a direct table for the Basic Multilingual
  Plane instead of a hash probe. 8.2 → 6.1 µs per frame on mixed scripts.

Every shader change was checked to produce pixel-identical screenshots.

### Round 3: startup, long lines, idle

- **File loads on its own thread** while SDL and AppKit start. Opening a
  1 GB file no longer adds 30 ms to startup.
- **libstdc++ and libgcc are linked statically.** Time before `main` went
  from 13.5 ms to 8.3 ms.
- **Column anchors for long lines.** Scrolled far into a very long line,
  each frame had to walk from the line start to find the first visible
  column (1.3 ms at column 60 M). The renderer now keeps an anchor every
  65,536 columns of the long lines it draws: 1.8 µs.
- **Idle really is idle.** The idle benchmark now waits for the new window to
  settle before measuring: 0 frames, 0.04% CPU over 3 s.

### Round 4: settings file, fonts by name, ligatures

New features, measured so that they cost nothing when unused and little when
used.

- **Startup did not move.** Finding the font by name, loading HarfBuzz and
  preparing the atlas all happen on a background thread while the window is
  being created. Interleaved launches: 114.7 ms with no settings file against
  115.5 ms with Rosé Pine, Victor Mono and ligatures. The main thread's share
  of font setup went from 0.28 ms to 0.04 ms.
- **HarfBuzz is loaded on demand.** Linking it normally added 1.5 ms before
  `main` to every launch (8.6 → 10.1 ms), ligatures or not.
- **`std::filesystem` was costing 2 to 3 ms per launch.** Using it to list
  font folders pulled the C++ locale machinery into the statically linked
  runtime: the binary grew to 2.4 MB and time before `main` to 10.4 ms.
  Plain directory calls brought that back to 7.3 ms.
- **Dead code is stripped.** GCC on macOS also left its LTO bytecode in the
  output. The binary went from 3.7 MB to 0.48 MB.
- **Ligature pass.** With ligatures on, each row of cells is scanned for runs
  of characters that can join, and runs are looked up in a cache. Checking
  every other cell is enough to find every run (a run has two neighbours, so
  one sits at an odd index), which cut the pass from 2.50 to 2.07 µs per
  frame on code and from 6.36 to 4.11 µs on the log file.

| Text to cell grid, one frame (290×82 cells) | Ligatures off | Ligatures on (Victor Mono) |
|---|---:|---:|
| 100 MB code file | 1.29 µs | 2.16 µs |
| 1 GB log file | 1.55 µs | 4.29 µs |

With ligatures off the path is the same as before; it measured 1.29 µs
against 1.18 µs in the previous round, which is inside the spread between
sittings on this machine.

`bench/run.sh` now sets `BLIT_CONFIG=/dev/null`, so results never depend on a
personal settings file. Set `BLIT_BENCH_FONT="Victor Mono"` (or any font with
ligatures) to include the ligature rows.

### Round 5: syntax highlighting with tree-sitter

The rule for this feature was that it may not slow the main thread. All
parsing and querying happens on a worker; the numbers below are split into
what the main thread pays and how long colours take to follow.

Measured on blit's own C++ source: 268 KB as is, and repeated to 2.7 MB.

**Main thread (what typing and scrolling feel):**

| | Without | With highlighting |
|---|---:|---:|
| Text to cell grid, one frame | 1.36 µs | 2.95 µs |
| Extra work per keystroke (hand-off to the worker) | | 2 to 3 µs |
| Launch to first frame, 45 KB file | 114.4 ms | 114.5 ms |
| Launch to first frame, 2.7 MB file | 117.2 ms | 117.4 ms |
| Late frames in a 4 s smooth scroll | 0 to 1 | 0 |
| Idle | 0 frames | 0 frames |

**Worker thread (how soon colours follow):**

| | First version | Final |
|---|---:|---:|
| Open to first colours, 268 KB | 125 ms | 55 ms |
| Launching on a 2.7 MB file: colours arrive | 90 ms after the first frame | in the first frame |
| Keystroke to colours on the visible lines, 268 KB | 6.3 ms | 0.9 ms |
| Same, 2.7 MB | 15.9 ms | 4.9 ms |
| One highlight pass over ~900 lines, 268 KB | 5.6 ms | 2.5 ms |
| Same pass in the middle of a 13 MB file | 34.9 ms | 2.3 ms |

What changed between the two columns:

- **Parent lookups.** Over half of a highlight pass went to the query tests
  `#has-parent?` and `#has-ancestor?`: tree-sitter finds a node's parent by
  searching down from the root, every time, and that search grows with the
  file. A tree cursor that stays near the last node asked about makes it a
  step or two. 5.6 → 2.5 ms per pass, and no longer dependent on file size.
- **Visible lines first.** After an edit the worker recolours the lines on
  screen (a fraction of a millisecond), sends that, and then redoes the
  wider range around them. If a newer edit arrives meanwhile, the wide pass
  is abandoned.
- **Compiling the query off to the side, in pieces.** Compiling the C++
  highlight query takes about 100 ms (Python 14, C 8, Lua 1), and tree-sitter
  cannot do it in parallel. It now compiles while the file is being parsed,
  and also in four slices on four threads (43 ms), which are used until the
  whole query is ready. Compiled queries are kept, so a second C++ file
  starts at once.
- **A quick first look at big files.** Text up to a point parses the same
  whatever follows it, so for a big file the worker first parses just the
  beginning, colours the first screen, and then does the full parse. The
  full parse waits until the first frame is out, so it does not compete with
  the window coming up (that cost 8.6 ms of launch time before). Measured at
  launch, files from 45 KB to 13 MB all had their colours in the first frame.
- **No redraw for nothing.** When the wide pass only confirms what the quick
  pass showed, no frame is drawn.

Two things are tree-sitter's own and are left as they are, off the main
thread: the first parse runs at about 13 MB/s for C++, and an incremental
reparse after one typed character takes time proportional to the file
(0.5 ms at 268 KB, 4 ms at 2.7 MB, 25 ms at 13 MB, measured with tree-sitter
alone). That reparse is what sets the "2.7 MB" row above.

Checked along the way: highlighting kept up to date across random edits is
identical to highlighting the same text from scratch, and the sliced query
gives the same result as the whole one (both in the test suite, and as
pixel-identical screenshots across 14 files in 11 languages).

### Round 6: italic and bold

The rule: a styled frame may not cost more than a coloured one. Each of
italic, bold and bold italic gets its own block of 128 atlas slots after the
regular one, so an ASCII character in style `s` is slot `byte | s << 7`. The
grid was already or-ing a colour into each run of cells; the style rides in
the same value. Characters outside ASCII ask the font for a styled glyph,
out of line. The shader is unchanged.

All rows are back-to-back runs of the previous binary and the new one, taken
alternately in one sitting (290×82 cells, real C++):

| | Before | After |
|---|---:|---:|
| Text to cell grid, plain | 1.24 µs | 1.26 µs |
| Same, syntax colours | 3.17 µs | 3.18 µs |
| Same, colours and every token in one of the four styles | | 3.15 µs |
| Victor Mono with ligatures, colours | 4.8 µs | 4.6 to 4.8 µs |
| Same, every token in one of the four styles (worst case) | | 7.4 µs |
| Scrolling C++ in Rosé Pine and Victor Mono with ligatures (italic comments and keywords, bold builtins): grid fill, median / mean | 3.00 / 3.30 µs | 3.00 / 3.70 µs |
| Same with the theme's stock styles (italic variables too) | | 3.00 / 3.94 µs |
| Same: CPU per frame, median | 48 µs | 48 µs |
| Line lookup, typing, undo, newline scan, first parse, keystroke to colours | | all within 4% |
| Launch to first frame, default settings (italic and bold faces loaded) | 128.1 ms | 126.8 ms |
| Launch to first frame, Victor Mono with ligatures (same) | 115.1 ms | 116.7 ms |

The two launch rows are within noise: launching the same binary twice in the
same alternating way gave medians 4 ms apart. The italic and bold faces are
loaded on the font thread while the window is being created, and that thread
is done long before the window is:

| Font loading (font thread) | Regular only | + italic and bold | All four |
|---|---:|---:|---:|
| System font (bold from its weight axis) | 0.13 ms | 0.58 ms | 0.78 ms |
| Victor Mono with ligatures (three more font files) | 1.20 ms | 1.72 ms | 2.28 ms |
| Monaco (no italic or bold: slanted and thickened here) | 0.33 ms | 0.74 ms | 1.03 ms |

Found by measuring:

- **The ligature cache emptied itself in a loop.** Runs of joining
  characters are cached per style now, and a run ends where the style does,
  so styled text has several times more distinct runs. The cache used to be
  wiped when half full, which with four styles on screen happened every few
  frames, and every wipe meant shaping everything again: 28 µs a frame in the
  worst case. It now doubles instead. 28 → 7.4 µs. The tables that map
  glyphs and ligature slices to atlas slots had the same flaw in a milder
  form (they restarted the whole atlas when half full) and are now sized to
  the atlas, so only a truly full atlas restarts it.
- **The styled glyph lookup did not belong in the row loop.** With it
  inline, plain ASCII frames (which never reach it) got 4 to 9% slower, from
  the extra register pressure alone. Moved to a cold function that decodes
  the character again: back to within 2%.
- **One pass over a ligature run, not two.** Checking that a run is all one
  style was a loop of its own at first. The scan that finds the run now stops
  at a style change, and a cached run no longer copies its characters out
  before the lookup.
- **Launch timings need alternating order.** Always launching the old binary
  first and the new one second showed the new one 11 ms slower. Alternating
  which goes first made that vanish; it was the order. The comparison script
  alternates now, and runs one binary against itself as a control.

The full-suite file for this round (`r6-styles.jsonl`) was taken with the
machine in a slower state than round 5 (the old binary, measured alongside,
shows the same: 64 GB/s newline scan against 73 then, and the do-nothing SDL
program launches 15% slower). Its numbers are not comparable to earlier
files, and the "Where it stands" tables above are left as they were.

Building for this round also turned up a toolchain trap, recorded here
because it cost two machine freezes: with GCC on macOS, `-g` together with
link-time optimization makes the link run `dsymutil`, which on these
programs never finishes and takes all the memory there is. The benchmark
build therefore uses Release's flags exactly, with no `-g`.

### Round 7: language servers

Two rules. Whatever a language server does, the main thread does not wait
for it; and an editor with no server running, or with one that has nothing
to say, costs what it did before.

The server is a child process and one thread does all the talking to it.
The main thread queues a small message per edit and picks up results that
are already parsed. Popups are extra cell grids drawn by extra triangles.

**With no server (the previous binary against the new one, taken alternately
in one sitting):**

| | Before | After |
|---|---:|---:|
| Text to cell grid, plain | 1.21 µs | 1.21 µs |
| Same, syntax colours | 3.00 µs | 2.97 µs |
| GPU throughput at 2560×1600 (frames/s) | 19,360 | 19,090 |
| CPU per frame, median | 48 µs | 48 µs |
| Type one character | 21.0 ns | 21.5 ns |
| Hand-off to the syntax worker per keystroke | 2.95 µs | 2.95 µs |
| Launch to first frame, C++ file, clangd installed and enabled | 115.8 ms | 115.6 ms |

The server is started after the first frame is out, which is why launch does
not move (launching one binary twice gave medians 5 ms apart, so that row is
"no difference", not "0.2 ms faster").

**With a server (a test server that answers at once, so that what is
measured is blit):**

| Main thread | |
|---|---:|
| Starting a server (once, after the first frame) | 70 µs |
| A keystroke: the edit, the change message, asking for completions | 2.3 µs (0.36 µs with no server) |
| The change message alone, columns in bytes / in UTF-16 | 0.73 µs / 1.0 µs |
| Taking in a completion list of 5000 items | 0.13 ms |
| Narrowing 5000 items as a letter is typed | 0.05 to 0.2 ms |
| Laying out a frame with the list, its side panel and signature help all open | 1.9 µs (0.07 µs with none) |
| CPU per frame with those three popups open, median | 50 µs (48 µs with none) |
| GPU throughput with those three popups open | unchanged |

| Worker thread | |
|---|---:|
| Process start to the server ready | 7 ms |
| A completion of 5000 items: asked to ready | 5 ms, of which parsing 0.76 ms |
| JSON read / written | 1.4 GB/s / 0.9 GB/s |

With real servers the times are the servers': on a 20-line file clangd is
ready 150 ms after blit is launched (without a window) and its first
diagnostics are in after 330 ms; the two processes together peak at 245 MB.

Found by measuring:

- **One shader for text and popups cost every pixel.** The first version put
  the popup's margin test and outline, and the underline's wave, into the
  text shader. With no server running at all, GPU throughput fell 29%
  (17,900 → 12,700 frames/s) and the median CPU per frame rose from 48 to
  72 µs, because the frame loop waits on the GPU. Popups now have a fragment
  shader of their own, and an underlined cell sets the same "more than a
  glyph here" bit the cursor's cell sets, so a pixel of plain text runs
  exactly the code it ran before. Back to 19,100.
- **Underlines do not belong in the fill loop.** Checking each row for
  underlines while filling it made every fill 5 to 14% slower, with nothing
  to underline. They are laid over the finished cells in a pass that only
  runs when there are any. Back to within 1%.
- **Most items in a completion list replace the same word.** Working out
  that word's position once instead of 5000 times took the main thread's
  share of a big list from 0.49 ms to 0.13 ms.
- **The first completion benchmark measured the test server.** 518 ms for
  5000 items was the test server finding the same position 10,000 times; the
  client's part was 5 ms.

Checked along the way, in the test suite: after 150 random edits (multi-byte
characters, pastes, undo) the server's copy of the document is identical to
the editor's, for columns counted in bytes and in UTF-16, and for changes
sent one by one and as whole documents. A server that dies is noticed and
let go of. Beyond the test server it was tried with clangd,
lua-language-server, taplo, bash-language-server and
vscode-json-language-server; typescript-language-server would not start
without TypeScript in the project, and the basedpyright on this machine has
lost its Python.

### Round 8: grammars of blit's own

Until here the grammars and highlight queries were ones another editor had
installed. blit now fetches each grammar from its own repository at a fixed
commit, compiles it with the system's C compiler and keeps it with the
grammar's own highlight query (see `src/grammars.cpp`). Inside the editor
this runs on a thread of its own, one grammar at a time.

| | |
|---|---:|
| Fetch, build and install C and C++ | 2.4 s |
| All 25 known grammars | 20 s, 22 MB on disk, 400 MB peak |
| Main thread, checking what is installed (after the first frame) | a few file lookups |

The grammars' own queries are smaller than the ones used before (14 kinds of
token for C++ instead of 45), so the syntax numbers changed with them. From
this round on they are measured with these queries:

| 268 KB of C++ | Before | Now |
|---|---:|---:|
| Open to first colours | 55 ms | 35 ms |
| One highlight pass over ~900 lines | 2.5 ms | 1.6 ms |
| Keystroke to colours on the visible lines | 0.9 ms | 1.0 ms |
| Text to cell grid with colours, one frame | 3.0 µs | 3.4 µs |

(The last row moved because the new queries colour punctuation and
operators separately from their neighbours, so a row has more colour runs.)

### Round 9: side panel, frame cap, a look with the profiler

The side panel is one more grid of cells drawn by one more triangle, with
the text view cut to the area beside it. Folders are read on a thread of
their own.

| | |
|---|---:|
| Panel closed, against the build from before language servers (A/B) | fill 0 to 3%, line lookups the same, GPU throughput ±1% |
| Panel open, its share of laying out a frame when nothing in it changed | 0.11 µs |
| Panel open, offscreen throughput | 5% lower (one more draw) |
| Window scrolling, CPU per frame, old build / this one, alternating | 31, 43 µs / 24, 32 µs |

The first version asked the system for the open file's full path every
frame to pick it out in the tree: 11.5 µs a frame. It is now worked out when
the file changes.

**Frame cap.** A frame is never drawn sooner than one refresh of the display
after the last. With vsync the window system already holds each frame back,
and blit does not hold it back a second time; where it does not (other
present modes, or drawing with nothing to wait for), blit sleeps out the
rest of the refresh.

| | |
|---|---:|
| Scrolling in a window on the 160 Hz display | 159.9 frames a second, 0 to 2 late of 639 |
| Drawing as fast as the loop will go, in a window | 156 to 159 frames a second |
| The same into a texture with `--fps-cap 160` | 160 |
| Keypress to frame submitted, before and after | 240 µs, 240 µs |

**What `sample` shows** (`bench/profile.sh scroll bench/data/code-100m.txt`,
drawing into a texture as fast as it will go, 17,000 frames a second):

| Main thread | |
|---|---:|
| Waiting for a command buffer (`SDL_AcquireGPUCommandBuffer`): the GPU is the limit | 72% |
| `SDL_BeginGPURenderPass` | 6.6% |
| Text to cells (`fill_grid`) | 6% |
| `SDL_BeginGPUCopyPass` | 4.7% |

The copy pass is the Metal driver setting up an encoder for the cell upload.
It is skipped already when no cell changed, and there is no cheaper way to
upload within SDL_GPU. Typing with a language server attached shows one
`write` a keystroke (waking the server's thread) at 2% and nothing else new.

**Memory** (`vmmap --summary`, `leaks --atExit`):

| | |
|---|---:|
| A plain text file open | 37 MB |
| 268 KB of C++ with colours | 74 MB |
| Peak while starting (the do-nothing SDL program: 123 MB) | 177 to 189 MB |
| During the 17,000 frames a second benchmark (158 MB is GPU buffers waiting to be reused) | 401 MB |
| Leaks at exit | 0 |

The random-edit tests, and scripted sessions that create, rename and trash
files in the panel while a language server is attached, run clean under the
address and undefined-behaviour sanitizers. The full suite after this round
is `bench/results/r9-panel.jsonl`.

### Round 10: tabs, line numbers, indent lines, scrolling

Several files can be open, each in a tab with its own text, undo history,
highlighter and place in the file. The tab bar and the line numbers are each
one more grid of cells drawn by one more triangle, like the side panel.
Indent lines are flagged cells in the text's own grid. Sideways scrolling
stops at the widest line, and a touchpad pull past an end stretches and
springs back.

From this round on the suite's frame numbers are taken with the tab bar,
line numbers and indent lines switched off, so that they stay the numbers of
the text view alone and compare with earlier rounds; `scroll_full` is the
same scroll with all three on.

Old build against new, alternating, scrolling the 100 MB file into a
2560×1600 texture:

| | CPU per frame (p50) | Text to cells | Frames a second |
|---|---:|---:|---:|
| Round 9 build | 47 to 49 µs | 1.13 to 1.17 µs | 14,900 to 17,000 |
| This build, all three off | 48 µs | 1.17 to 1.20 µs | 16,400 to 16,900 |
| Tab bar on | 49 µs | 1.12 µs | 16,600 |
| Line numbers on | 49 µs | 1.17 µs | 16,300 |
| Indent lines on | 49 µs | 1.52 µs | 16,700 |
| All three on (the default) | 50 µs | 1.40 µs | 16,200 to 16,300 |

In a window nothing moved: a keypress reaches the GPU in 123 to 143 µs
(p50) on either build, and scrolling holds 159.9 frames a second with 0 or 1
late out of 639 with everything on.

| | |
|---|---:|
| Indent lines, a frame at a new place (indentation read off the cells) | +0.35 µs |
| Indent lines, the same rows again (remembered) | under 0.1 µs |
| Indent lines at a new place while scrolled sideways (indentation read from the text) | +4.9 µs |
| Guessing a file's indentation step (first 4,000 lines, once per file) | 26 to 31 µs |
| Widest line, 100 KB of code (measured on the spot) | 24 to 32 µs |
| Widest line, 1 MB of code | 0.22 ms |
| Widest line, 100 MB of code (on its own thread) | 22 ms, 4.4 GB/s |
| Widest line, 1 GB log | 90 ms, 11 GB/s |
| Widest line, one 64 MB line | 2.9 ms, 21 GB/s |
| Widest line, 16 MB of mixed-script UTF-8 | 27 ms, 0.6 GB/s |

The widest line is not measured until the view is first scrolled sideways,
and again only after the text has changed, so opening and editing cost
nothing for it. Most lines are ruled out by their length alone (a line of n
bytes is at most n × tab width columns), which a search for the newline
gives; only candidates are walked. Text outside ASCII is the slow case,
because there bytes overstate columns and more lines stay candidates.

**What `sample` shows** with all three on (`bench/profile.sh scroll
bench/data/code-100m.txt`): the main thread still spends 73% of its time
waiting for the GPU. `fill_grid` is 1.2% and `guide_grid` 0.8%; nothing else
of the tab bar, the line numbers or the tabs themselves reaches 0.4%.

**Memory** (`vmmap --summary`, drawing into a 1600×900 texture) and leaks
(`leaks --atExit` after a session that opens, drags and closes tabs, scrolls
past the ends and measures a 100 MB file):

| | |
|---|---:|
| One C++ file open | 48.8 MB |
| Eight C++ files in tabs, each with its own parser and colours | 58.0 MB |
| Those and a 1 GB log in a tab behind | 58.7 MB |
| Leaks at exit | 0 |

Launch time, old build and new alternating six times each: 144 to 162 ms
old, 147 to 169 ms new, with or without the new parts (the machine was
slower than in round 9 at this sitting: the do-nothing SDL program took 137
ms against 124). The random-edit tests, the new tests (widest line against a
plain walk, indentation guesses, where indent lines fall, the language
server moving between documents) and scripted sessions over all of this run
clean under the address and undefined-behaviour sanitizers. The full suite
is `bench/results/r10-tabs.jsonl`.

A later sitting had macOS handing windows a frame every 8.7 ms, 115 a
second, on the same display set to 160 Hz, on mains power and with no
thermal limit showing. The round 9 build and this one were paced alike
(115.0 and 115.5 a second, no late frames), so it is the window system's
doing and not blit's; the 160 figures above are from the earlier sitting.

Moving between tabs of the same language keeps the language server running:
the old document is closed on it and the new one opened, instead of a new
server being started each time.

### Round 11: splits

The window can be divided into splits, each with its own tabs. A split's
text is one more grid of cells in the same buffer, drawn by the same
triangle and shader as before, cut to the split's rectangle; the renderer
now takes a list of text views where it took one. What it remembers from
frame to frame (anchors into long lines, how lines are indented) is kept per
split and tagged with the document, so that two files cannot be mixed up.
(That also fixed a slip from round 10: after a change of tab the indent
lines could be the previous file's until the view moved.)

An unsplit window, round 10 build against this one, alternating three times,
scrolling the 100 MB file into a 2560×1600 texture:

| | CPU per frame (p50) | Text to cells | Frames a second |
|---|---:|---:|---:|
| Round 10, tab bar, numbers and indent lines off | 48 to 49 µs | 1.13 to 1.17 µs | 16,000 to 17,000 |
| This build, the same | 49 µs | 1.16 to 1.22 µs | 16,700 to 16,800 |
| Round 10, all three on | 49 to 50 µs | 1.41 to 1.48 µs | 16,300 to 16,500 |
| This build, all three on | 50 to 51 µs | 1.42 to 1.47 µs | 15,900 to 16,200 |

Split in two and in four, the other splits showing C++ files with colours
and standing still while the one with the keyboard scrolls (three runs
each):

| | CPU per frame (p50) | Text to cells | Uniforms and draws | Frames a second |
|---|---:|---:|---:|---:|
| One | 50 to 51 µs | 1.43 to 1.48 µs | 7.4 to 7.6 µs | 15,500 to 16,400 |
| Two splits | 52 µs | 2.67 to 2.76 µs | 8.0 to 8.2 µs | 15,700 |
| Four splits | 52 to 53 µs | 3.07 to 3.11 µs | 9.2 to 9.3 µs | 15,400 to 15,500 |

Each extra split costs about half a microsecond of drawing (its text, tab
bar and line numbers are three more draws) and about half a microsecond of
making cells, because a split that has not changed is still turned into
cells again every frame. Skipping that for a split whose text, view,
selection and colours are what they were would take the second half back;
it needs the highlighter to say when its colours last changed, which it does
not yet, so it is left for a later round.

`sample` on the four-split run (`bench/profile.sh scroll ... --script ...`)
has the main thread waiting for the GPU 69% of the time, `fill_grid` at
3.1% and `guide_grid` at 0.6%; nothing of the splits themselves shows.
`leaks --atExit` after a session that makes splits, drags a file, a tab and
a divider and closes them again reports 0 leaks. The full suite, which now
ends its frame section with the four-split scroll (15,800 frames a second,
52 µs of CPU a frame), is `bench/results/r11-splits.jsonl`; against round
10 nothing else moved by more than the run-to-run spread.

The split cases themselves (dragging tabs, groups and files onto edges,
middles and tab bars, the divider, the keyboard) are a test now:
`tests/splits.sh` sends the mouse and key events through the real handlers
and compares the arrangement that results.

### Round 12: git

Git is done by running the `git` program on one background thread, which
also compares each open document with its committed text for the marks
beside changed lines. The editor's thread queues a request and later takes
an answer that is already parsed.

Round 11 build against this one, alternating:

| | CPU per frame (p50) | Frames a second |
|---|---:|---:|
| Round 11, scrolling a file in no repository | 51 µs | 18,000 to 18,700 |
| This build, the same | 50 to 51 µs | 17,400 to 18,000 |
| This build, a file with changes in a repository, git off | 50 to 53 µs | 17,000 to 19,100 |
| The same, git on (marks in the line numbers) | 50 µs | 18,900 to 19,000 |

| | |
|---|---:|
| Launch to first frame in a repository, round 11 / git on / git off (median of 12 each, alternating) | 116.7 / 115.6 / 113.6 ms |
| Idle for 3 s in a repository, git on, blame line off: frames, CPU | 0, 0.04% |
| The same with the blame line on | 2 (the marks and the blame appearing), then none |

Nothing of git starts until the first frame is out, and an answer that
changes nothing on screen draws no frame.

What the background thread spends:

| | |
|---|---:|
| `git status`, `for-each-ref`, `log -n 40` (one refresh: all three) | 7.4, 6.8, 7.1 ms |
| `git show HEAD:file` (once per file per commit; kept as line numbers) | 6.9 ms |
| `git blame` of a 1 MB, 36,000-line file | 26 ms |
| A number per line, 1 MB / 100 MB | 0.46 ms / 26 ms (2.1 to 3.7 GB/s) |
| Comparing 1 MB with its committed text, twenty changes / none | 18 µs / 8 µs |
| The same for 100 MB (the editor stops comparing at 16 MB) | 1.4 ms / 0.8 ms |

The comparison is Myers' algorithm on the line numbers, after cutting off
what the two texts share at either end; its result is checked in the tests
against the longest common subsequence (it must be the fewest changes
possible). While typing, one comparison at a time is asked for per document,
the next when the last has answered; a blame is asked for 0.4 s after the
text last changed.

The tests run the real thing: `tests/git.sh` makes scratch repositories with
a "server" that is a folder beside them, drives the editor through edit,
stage, commit, push, fetch, pull, branch, merge, a conflict and its abort,
and discard, and compares what git then tells the editor; it also checks
that a file changed on disk by a pull or a change of branch under an open
tab is read again (an edit made after it must not undo the pull). All of it
runs clean under the address and undefined-behaviour sanitizers.

### Round 13: modal editing

Vim's modes and Neovim's default keys, as a layer over the text engine
(`src/vim.cpp`) that knows nothing of windows: keys go in, and what is not
the text's business (save, close, split) comes back out as a request. The
text engine gained one thing for it, undo steps that go together: a command
that changes several places, or everything typed in one stay in Insert mode,
is one step.

Round 12 build against this one, alternating three times, scrolling the 100
MB file:

| | CPU per frame (p50) | Frames a second |
|---|---:|---:|
| Round 12 | 50 to 51 µs | 15,800 to 16,200 |
| This build, modes off | 51 µs | 16,000 to 16,200 |
| This build, modes on (the bottom line is one more small draw) | 51 µs | 16,000 to 16,200 |

What a key costs on the editor's thread, from the text event arriving to the
edit done and the view following the cursor (`--bench keys`, an event a
frame through the real handlers):

| | |
|---|---:|
| Typing, modes off | 0.16 to 0.17 µs |
| Typing in Insert mode | 0.24 to 0.26 µs |
| Normal mode, moving (`w`, `b`, `j`) | 0.78 µs |
| Normal mode, deleting (`x`, `dw`) | 0.71 to 0.77 µs |

The first Normal-mode figure was 0.95 µs. Vim counts lines without the
empty one after a final newline, which means looking at the last byte of the
file, and that was done three times a key: on a 100 MB file a read at the
far end is most of a microsecond's worth of cache misses. It is now kept
until the text changes.

Search matches are lit by flagging their cells, on the same "more than a
glyph" bit as the cursor, squiggles and indent lines, so the shader's path
for ordinary text is as it was; which cells match is worked out again only
when the text, the view or the pattern changes.

The tests: 170 cases of "this text, these keys, that text and cursor" taken
from how Vim behaves, the modes and messages, searching across a text
larger than one window of the scan, and `tests/modal.sh`, which sends real
key and text events and checks what reaches Vim and what the application
does when Vim asks it to save, close and split.

### Round 14: the rest of Vim's Normal, Visual and Insert modes

Asked whether all of Neovim's base Visual and editing behaviour was there,
the answer was no. Added: Visual Block mode (Ctrl-V, with `I A c C D r ~ u U`
`> <`, `$`, `o O`, and rectangles that go back in as rectangles), the jump
list (Ctrl-O, Ctrl-I, ``` `` ``` and `''`), the marks Vim keeps (`` `. `^ `< `> ``),
marks that move with their text, the numbered and small-delete registers,
`". "/ ":`, sentence motions and objects, tag objects, `[( [{ ]) ]} [[ ]]`,
counts on text objects, a search as an operator's motion and in Visual
mode, `gn`, `gi`, `gp`, `g?`, `g*`, `&`, `Q`, counts on `o` and `O`, the
Visual-mode keys for whole lines (`X D Y S R C`), `I A * #` there, text
objects that grow a selection, and in Insert mode Ctrl-T Ctrl-D Ctrl-A
Ctrl-V Ctrl-E Ctrl-Y and Replace mode's Backspace. A fault found on the way:
after Ctrl-O in Insert mode a command that itself starts Insert mode (`I`,
`cw`) left an undo step open, so everything typed afterwards would have
undone as one.

An ordinary frame pays nothing for any of it, and an ordinary key next to
nothing (measured below). The two places where it touches those paths:

- **The rectangle** is drawn with what was there. A selection used to be one
  run of text, which a rectangle is not; rather than teach the shader a
  second kind of selection, each visible line of the block is one more
  entry in the list that already carries squiggles and search matches,
  with a kind whose only effect is to set the "selected" bit of the cells
  it covers. The shader is unchanged, and a frame with no block pays
  nothing. The first version worked out in the application how much of the
  rectangle each visible line has text for, a tree lookup and a scan of the
  line per row: 10 µs a frame with a block on screen (below). But the grid
  has just laid those lines out and knows where each one ends, since the
  cells after the text hold no glyph. Clipping there instead is a walk back
  over a few cells already in the cache.

  | Scrolling 100 MB, 52 rows on screen | Layout | Filling the grid | CPU per frame (p50) |
  |---|---:|---:|---:|
  | Nothing selected | 0.47 µs | 1.42 µs | 51 µs |
  | Visual mode over all of it | 0.65 µs | 1.85 µs | 51 to 52 µs |
  | Visual Block over all of it, lines measured in the application | 10.3 µs | 1.46 µs | 52 to 53 µs |
  | Visual Block, clipped by the grid | 1.09 µs | 1.71 µs | 51 to 52 µs |

  `sample` on the main thread of that last run: 61% waiting for the GPU,
  `fill_grid` 0.8%, `decorate_grid` 0.8%, and nothing of Vim's among the
  functions it lists.

- **Marks that move with the text** need to hear about every edit. The two
  listeners the editor already has (syntax, language server) are given
  rows and columns, which cost two tree lookups an edit to work out; marks
  need only the three offsets the edit already has in hand. So there is a
  separate hook, `EditWatch`, that passes just those: a branch on a null
  pointer when nothing watches, and a pass over the marks and the jump
  list (at most a hundred numbers) when Vim does.

Round 13 build against this one, six turns each, alternating, 100 MB file,
2560x1600 offscreen. Medians, with the range over the six:

| | Round 13 | This build |
|---|---:|---:|
| Typing, modes off | 0.28 µs (0.26 to 0.29) | 0.29 µs (0.26 to 0.30) |
| Typing in Insert mode | 0.24 µs (0.24 to 0.26) | 0.28 µs (0.26 to 0.29) |
| The same with two marks and three jumps to keep in place | 0.32 µs (0.30 to 0.34) | 0.35 µs (0.32 to 0.36) |
| Normal mode, moving (`w`, `b`, `j`) | 0.88 µs (0.82 to 0.90) | 0.81 µs (0.80 to 0.83) |
| Scrolling: CPU per frame (p50) | 51 µs | 51 µs |

(The typing figures with modes off are higher than Round 13's 0.16 to 0.17
µs for both builds: the machine at this sitting, not the code. The timer
counts whole microseconds, so these are averages of mostly zeros and ones.)

Typing in Insert mode did get dearer, by 0.01 to 0.03 µs a key depending on
the sitting (an earlier six turns had the two builds level at 0.26). To
see what of that is the watch, a build without it, 20,000 keys a run, ten
turns of the three builds in rotation:

| | Median | Range |
|---|---:|---:|
| Round 13 | 0.48 µs | 0.46 to 0.51 |
| This build without the watch | 0.49 µs | 0.47 to 0.50 |
| This build | 0.50 µs | 0.48 to 0.51 |

So about 0.01 µs a key is marks being kept in place and as much again came
with the rest (Insert mode now tells more keys apart). Left there: it is
two parts in a hundred thousand of a 160 Hz frame, and a mark that points
at the wrong line after an edit is a fault, not a saving.

The tests: the table of "this text, these keys, that text and cursor" went
from 170 cases to 318, the rectangle itself is checked directly, and
`tests/modal.sh` has ten more cases through the real key events (Ctrl-V
reaching Vim, Tab as Ctrl-I, the jump list surviving a visit to another
file, a mark staying with its line).

Still not there, and the largest gap: `/` and `:s` look for what is typed
as it stands, not as a regular expression. The README lists the rest.

### Round 15: Neovim's defaults, checked against its documentation

Asked whether the keys were the same as base Neovim's, the honest answer
was "as Vim's, mostly", which is not the same thing. Neovim's list of what
it does differently from Vim (`vim_diff`, the default mappings and the
option defaults) was read through and the keys that behaved differently
were changed: `G gg H M L`, `:{number}`, `dd`, `>>` and `<<` now keep the
column (`'startofline'` is off there), Space and Backspace go on to other
lines, `&` keeps the flags, `v$` takes the end of the line, `P` over a
selection keeps the register, Ctrl-A and Ctrl-X know hexadecimal and binary
and keep leading zeros. And the default keys that were missing and could be
had: `gc` and `gcc` (comments, by Neovim's algorithm), `]d [d ]D [D`,
`[b ]b [B ]B`, `[<Space> ]<Space>`, Ctrl-T, `@` and `Q` on each line of a
Visual Line selection, Ctrl-S in Insert mode for signature help, and Ctrl-U
and Ctrl-W there as an undo step of their own.

Nothing here is on a frame's path. Round 14 build against this one, six
turns each, alternating (medians and ranges):

| | Round 14 | This build |
|---|---:|---:|
| Typing, modes off (code this round did not touch) | 0.31 µs (0.30 to 0.33) | 0.29 µs (0.27 to 0.31) |
| Typing in Insert mode | 0.29 µs (0.28 to 0.31) | 0.29 µs (0.27 to 0.32) |
| The same with two marks and three jumps to keep in place | 0.37 µs (0.35 to 0.40) | 0.37 µs (0.35 to 0.38) |
| Normal mode, moving (`w`, `b`, `j`) | 0.85 µs (0.82 to 0.89) | 0.85 µs (0.82 to 0.88) |
| Scrolling: CPU per frame (p50) | 51.5 µs | 51.5 µs |

The first row is the measure of the noise: that path does not reach Vim at
all, and it moved more than the rows that do (an earlier six turns had it
the other way round, 0.29 against 0.31).

The tests: 363 cases in the table (twelve older ones had Vim's cursor
position after `gg`, `dd` or `>>` and now have Neovim's), 24 for comments
with the markers of several languages, and `tests/modal.sh` at 56 cases
through real events, among them `gcc` in a C file and a Python file and
`]d` and `[d` against the test language server's diagnostics.

### Round 16: looks and motion

Round corners, shadows, rounded highlights, things that light up under the
mouse, and motion (popups fade in, the cursor glides, the scroll thumb
fades away). All of it is drawn by the two fragment shaders that were
there: a rounded box is a distance function, a shadow is the same function
with a soft edge, a rounded highlight is a run of cells that knows where it
starts and ends. No new geometry, no textures, no second pass.

The first version cost 14% of the frame rate with every bit of it switched
off. This shader does so little per pixel (find the cell, read it, maybe
read the atlas) that anything added shows, used or not: the GPU's compiler
turns a short `if` into arithmetic that every pixel does. Measured by
taking parts out, scrolling the 100 MB file at 2560x1600, nothing styled:

| | Frames a second |
|---|---:|
| Round 15 | 18,350 |
| New shaders, nothing switched on | 15,700 |
| ... without the code that rounds a selection's corners | 16,990 |
| ... and without the one line that dims a view | 18,120 |
| ... and with the old code for boxes | 18,450 |

So the shaders are built more than once from one source (`#ifdef`): the
text's plain, with rounded selections, and with dimming too; the boxes'
plain and styled. A view or a box is drawn by the build that does no more
than it asks for, and a frame that asks for nothing is the old frame, pixel
for pixel and at the old speed.

What it costs switched on was then brought down in three steps, each
measured:

| Scrolling, frames a second | Round 15 | First | Flags from the CPU | Final |
|---|---:|---:|---:|---:|
| The default look | 18,450 | 17,510 (-5.3%) | 17,790 | 17,750 (-3.8%) |
| With the side panel open | 17,790 | 15,210 (-15.5%) | 16,770 | 17,200 (-3.3%) |
| With everything on screen selected | 18,510 | 11,180 (-39%) | 15,510 | 16,200 (-12.4%) |

- **No pixel looks at its neighbours.** Rounding the corner of a selection
  means knowing whether the cells beside and above are selected, and the
  first shader read them: two more buffer reads for four pixels in ten. Now
  the CPU marks, in four spare bits of a cell, which of its corners the
  outline turns at. It does not scan for them: it knows where the selection
  starts on each row and walks along it, so only the rows a selection is on
  are touched, and of those four cells each (`round_selection()`, 0.55 µs a
  frame with the whole screen selected). The same for a lit row in a list:
  two bits say where the run starts and ends.
- **A box that hides what is under it is not blended.** A shadow has to be
  laid over what is there; the box that casts it does not, and blending all
  of a side panel so that its edge can cast a shadow was most of the 15%.
  The box is now written straight and only the strips of shadow around it
  are blended. Pixels in a shadow skip the cells altogether.
- **Rows with nothing lit get the plain shader.** A side panel is tall and
  has two or three lit rows; it is drawn in bands, the lit ones by the
  styled shader and the rest by the plain one.

What is left of the default look's 3.8% is not pixels but calls: the strip
of shadow under the tab bar is one more draw with another pipeline, and a
draw costs about a microsecond of CPU whatever its size (with shadows off
and rounding on: -2.1%; CPU per frame 52 against 53 µs). A split dimmed by
`dim_inactive` costs 9.6% while it is on screen, which is why that is off
unless asked for.

With the look switched off in the settings: 18,490 against 18,530 frames a
second. What a key costs on the editor's thread is unchanged (typing in
Insert mode 0.30 µs against 0.30, Normal-mode moves 0.86 against 0.84).

The motion is tested by its end: a frame drawn after the cursor has
glided and the menu has faded in must be the frame drawn without motion,
pixel for pixel (`tests/look.sh`, which also checks that the same script
gives the same frame twice, that the settings change it, and that the
mouse over a tab or a row lights it and leaving puts it out). Taking the
screenshots found a fault from Round 12: Cmd+Option+B, the key for git's
blame column, was taken by Cmd+B and hid the side panel instead.

The release binary is 1.65 MB (1.55 before): three more builds of the
shaders in two formats each, and the code.

### Round 17: finding files and text

One box in the middle of the window finds things under the working
directory: a file by its name (Cmd+P), a line by what is in it
(Cmd+Shift+F), and a path typed out with the folder's entries listed under
it (Cmd+O, naming a file that has none, choosing a folder). The last takes
the place of the system's file dialogs, which are gone. Names and text are
not searched on the editor's thread: a worker reads the tree and makes the
searches, and a search that has been overtaken by another keystroke is
dropped. (The path box lists the one folder typed, there and then.) The
raw numbers are in `bench/results/r17-find.jsonl`.

**Names.** The folder is read once into one string of paths (what a
`.gitignore` leaves out is left out, and so are build folders and compiled
files). What is typed is split at spaces into words, and every path is
scored against every word the way fzf does it: the letters in order but not
necessarily together, more for a letter that starts a word or follows the
one before it, less for gaps, more again if the word is all in the file's
name. A path is scored twice, with the letters laid as early as they come
and as close together as they go, and the better one counts (the closest
fit alone put a match in the middle of a name above one at its start).

| Ranking 200,000 paths (6.9 MB) on one thread | |
|---|---:|
| Two letters typed (11,123 paths match) | 8.4 ms |
| Seven letters | 8.5 ms |
| Four words | 12.5 ms |
| Nothing matches | 8.4 ms |

That is 42 ns a path, and it was left on one thread: a tree of 37,757 files
is ranked in 2.2 to 3.9 ms, inside one frame at 160 Hz.

**Text.** The files are read whole, by up to eight threads, and each is
searched for the longest word typed; the other words are looked for only on
the lines that one is on. What has been found is shown every 40 ms, so the
first lines are on screen before the search is over, and the search stops
at 3,000 lines.

How a word is found in a file is the part that was measured more than once
(100 MB of code in memory, one thread):

| | GB/s |
|---|---:|
| macOS's `memmem`, a word typed with a capital (matched as typed) | 2.1 |
| `memchr` for the word's first letter, then compare; a word that is not there | 8.0 |
| `memchr` for the word's rarest letter; the same word | 22.3 |
| ... a word typed with a capital | 63.9 |
| ... a word of common letters ("tensor") | 2.5 |
| ... two words | 2.7 |
| ... "return": 421,115 lines found, 96 ns each | 2.4 |

`memchr` is the fast part, and how much of a search is spent in it depends
on how seldom it stops. So it is given the letter of the word that stops
it least: a table ranks the bytes
by how common they are in code and prose, and the search looks for the
rarest one in the word and compares the rest around it. A word that takes
either case is looked for as two bytes (the small letter and the capital),
each from where it was last found. A word with nothing rare in it gains
nothing; most words that are searched for have something.

On disk (the folder already in the system's cache):

| | macOS SDK: 37,757 files, 334 MB | blit's own tree: 115 files |
|---|---:|---:|
| Reading the folder | 176 to 225 ms | 0.24 to 0.35 ms |
| Finding a name | 2.2 to 3.9 ms | 0.005 to 0.008 ms |
| Finding a rare word in all of it | 270 to 285 ms | 0.65 to 1.0 ms |
| Finding "return" (stops at 3,000 lines) | 8.8 to 9.0 ms | |

**What it costs when it is not in use.** Scrolling with the box closed:
17,650 to 17,780 frames a second before, 17,780 after (two sittings). With
the box open over the scrolling text: 11,700, and 81 µs of CPU a frame
against 53 (it is the largest popup there is).

Typing was dearer, though nothing on the way from a key to the text calls
the finder:

| A key on the editor's thread, µs (14 runs each, old and new turn about) | Round 16 | First | Final |
|---|---:|---:|---:|
| Modes off | 0.295 | 0.385 (0.30..0.41) | 0.310 |
| Insert mode | 0.280 | 0.395 (0.28..0.42) | 0.280 |

The runs of the first version fell into two groups, 0.31 and 0.40. All
that every key did was ask whether the box was open, and the answer was
kept with the rest of the finder's state, behind a pointer, in a function
in another file. Moving the flag into the application object, which every
key reads anyway, and not calling a closed finder at all, took the whole
difference away. Why one read through a pointer cost 90 ns in some runs
and nothing in others was not pinned down; a cache miss that depends on
where the allocation fell is the likely reason, and it is a guess.

A second sitting on the finished build, six runs each: 0.300 against 0.305
with modes off, 0.285 against 0.290 in Insert mode, 0.840 against 0.835 for
Normal-mode moves. The other things a key asks about (the side panel, the
popups) keep their flags behind pointers of their own and may hide the same
cost; old against new only shows what a round added.

`tests/find.sh` drives the box through the real key and text events in 36
cases (names, text, `:find` and `:grep`, the path box opening a file,
naming a new one and choosing a folder, the ignore rules, with Vim's modes
on and off), and the fuzz binary checks the matcher, `.gitignore`'s
patterns, the folder walk, the search of a text and the worker (a folder
read again while a search is wanted). The release binary is 1.78 MB (1.65
before).

### Round 18: scripts and plugins in Lua

blit runs Lua 5.5: `init.lua` beside the settings file, modules from
`lua/` there, `plugin/` files, and plugins (folders under the data folder,
fetched with git by `:PlugInstall`). Scripts reach the editor through the
table `blit`: every setting, mappings in each of Vim's modes, commands,
events by Neovim's names, the documents, timers, programs run in the
background, floating boxes, the finder box as a picker, marks over the
text, and messages. README.md lists it all.

**Nothing when there is nothing.** Without `init.lua` and plugins there is
no Lua in the process: the state is made only when there is something to
run (or when `:lua` is typed). What scripting adds to every key is a test
of one flag that is a member of the application (the lesson of Round 17):

| A key on the editor's thread, µs, no scripts (6 runs each, old and new turn about) | Round 17 | Round 18 |
|---|---:|---:|
| Modes off | 0.325 | 0.330 |
| Insert mode | 0.300 | 0.290 |
| Normal-mode moves | 0.800 | 0.795 |
| Normal-mode jumps | 0.335 | 0.350 |
| Scrolling, frames a second | 14,960 | 15,460 |

(Scrolling is slower than in Round 17's sitting on both builds alike; see
"Reading these numbers".)

**With scripts.** A usual `init.lua` (twenty mappings, two commands, a few
settings) against none, and the same with handlers for the events that
happen on every key (`TextChanged`, `TextChangedI`, `CursorMoved`,
`CursorMovedI`, `ModeChanged`), each an empty Lua function:

| A key, µs (6 runs each, turn about) | No scripts | init.lua, first | init.lua | And handlers on every key |
|---|---:|---:|---:|---:|
| Typing in Insert mode | 0.310 | 0.345 | 0.335 | 1.105 |
| Normal-mode moves | 0.835 | 0.865 | 0.840 | 1.430 |

The first version read the clock twice after every key (for timers and
for messages that go away), whether or not there were any; now the clock
is read only when something keeps time, and a usual `init.lua` costs a key
nothing that can be measured. A handler that runs costs 0.4 to 0.6 µs a
call: most of it is the table the handler is given and the protected call.
Typing in Insert mode with two such handlers is 1.1 µs a key, against
50 µs for the frame that follows it.

Inside Lua, timed by Lua (each many thousand times):

| | µs |
|---|---:|
| A key that is mapped to an (empty) function, typed by `blit.feed` | 0.15 |
| A key that is not mapped, typed by `blit.feed` | 0.13 |
| `blit.cursor()` | 0.034 |
| `blit.lines(0, 1, 50)` | 4.4 |
| `blit.set_lines`, one line | 0.28 |
| `blit.mark` | 0.10 |
| `blit.opt.editor.tab_width`, first | 0.35 |
| `blit.opt.editor.tab_width` | 0.19 |
| An empty Lua function | 0.014 |

Reading a setting made a new table for `editor` each time; the tables for
the sections are kept now.

**Startup.** Making the Lua state, loading its libraries and running
blit's own Lua (the part of the API written in Lua: settings, the plugin
manager) takes 0.37 ms; with the usual `init.lua`, 0.43 ms (7 runs each).
blit's own startup work is about 9 ms of the 125 ms from `exec` to the
first frame. `init.lua` runs before the window exists, so that settings
such as the font and the window's size are known when they are needed;
what it asks for that needs the window (`blit.open`, `blit.cmd`) is done
once the window is there.

**How it stays safe.** Lua reports an error by jumping (`longjmp`) out of
whatever C was running, which in C++ skips destructors. So no function Lua
calls lets it raise an error while a C++ object of its own is alive: it
reads arguments with calls that cannot fail, and returns what went wrong
for a wrapper to raise afterwards. A script that runs longer than
`script.timeout_ms` is stopped by a hook that looks at the clock every
10,000 instructions. Errors show at the bottom right with the place in the
script (`init.lua:12: ...`), tracebacks go to `:messages`, and the editor
carries on. A mapping that types itself stops 20 deep, and a save made by
a handler of a save fires no handlers.

**Mappings** sit in front of everything else that takes keys. A key that
may begin a mapping is held back until the rest come or
`keys.timeout_ms` passes; then it goes on as it would have. Option and a
letter type a character as well as press a key: a mapping that takes the
press takes that character too. A typed text event with several
characters is looked at one character at a time, in the mode the ones
before left (`ihellojk` with `jk` mapped to Esc leaves Insert mode at the
`k`).

**Events** that blit is told of (a file read, written, closed) are run at
the next turn of the loop, after the input that caused them; the ones that
are noticed (another file in front, the text or the cursor changed) are
looked for after each batch of input; and every change of mode is noted as
Vim makes it, so that `InsertEnter` and `InsertLeave` are not lost when
several keys come in one batch. `BufWritePre` runs before the file is
written, so what it changes is saved. Everything a script call changes in
a document is one undo step.

The tests found these, each fixed: a capital with Ctrl was read as Shift
(Vim reads `<C-S>` as `<C-s>`, but `<M-J>` as Alt with Shift); after Insert
mode no `TextChanged` came (Vim keeps one count of changes for Insert mode
and one for the rest, and so does blit now); a job killed just as it
started was killed only 300 ms later, the hard way; timers that were all
late ran in the order they were made, not the order they were due, and one
that repeats ran again at once to catch up.

`tests/lua.sh` has 46 cases through the real key and text events
(settings, mappings in each mode, commands, events, timers, programs, the
documents, pickers and boxes, plugins, `:PlugInstall` from a repository in
a scratch folder, `init.lua` saved and run again, scripts that fail or never
end), and the fuzz binary 32 checks of the notation, the mappings, every
setting a script can read (set back to what it reads, it must be accepted),
settings one at a time, and programs. All of it passes under the address
and undefined-behaviour sanitizers.

The release binary is 2.26 MB (1.78 before); most of that is Lua itself.

### Round 19: a Metal backend of blit's own

SDL_GPU is gone. blit draws through a small GPU interface of its own
(`src/gpu.h`, in C): Metal on macOS (`src/gpu_metal.m`, Objective-C built
by Apple's clang next to GCC's C++), and Vulkan on other systems
(`src/gpu_vulkan.cpp`, every call of which fails for now). SDL still has
the window and input. The frames are the same pixel for pixel: seven
scenes (plain text, a rounded selection, the side panel with its shadows,
the finder box, a dimmed split, a Visual Block selection, search matches)
drawn by both builds compare equal byte for byte.

**Why.** A profile of SDL_GPU's frame, paced as a window paces it, put 21%
of the main thread's busy time under one call, `SDL_BeginGPUCopyPass`: to
copy the cells to the GPU, the Metal driver makes a blit encoder every
frame, and making one allocates a large block and clears its pages
(`madvise`). SDL_GPU has no way around that, since a shader can read only
a buffer a copy pass filled. Natively the cells are written by the CPU
straight into memory the GPU reads (one buffer for each frame under way),
and the copy is gone. Uniforms go into the commands (`setFragmentBytes`),
the command buffer does not keep what it uses alive (blit does), and the
atlas is copied to only on frames that add glyphs.

Old against new, turn about, 2560x1600 (and the window benchmarks in a
real window on the display at hand, 60 Hz):

| The benchmark settings (5 runs each) | SDL_GPU | Metal |
|---|---:|---:|
| Back to back, frames a second (the GPU's limit) | 18,020 | 18,270 |
| CPU per frame, paced at 160 a second | 27.0 µs | 18.0 µs |
| … of which in the GPU interface | 27.3 µs | 15.6 µs |
| CPU per frame, paced at 1,000 a second | 40.0 µs | 28.0 µs |
| A key press to its frame handed to the GPU, p50 | 96 µs | 60 µs |
| … p99 | 176 µs | 101 µs |
| CPU per frame in a window | 40.0 µs | 31.4 µs |

| Everything on (4 runs each; see below) | SDL_GPU | Metal |
|---|---:|---:|
| Back to back, frames a second | 17,120 | 16,720 |
| CPU per frame, paced at 160 a second | 35.5 µs | 28.5 µs |
| Typing with completions, back to back: CPU per frame, mean | 53.0 µs | 56.7 µs |
| A key press to its frame handed to the GPU, p50 | 110 µs | 80 µs |
| … p99 | 274 µs | 476 µs |
| CPU per frame in a window | 52.5 µs | 43.9 µs |

"Everything on" is how blit is used, not the stripped settings the rest of
this file mostly uses: a 2,900-line C++ file (`vim.cpp`) coloured by
tree-sitter, clangd running with its diagnostics in, git with changed
lines marked and a blame, the Rosé Pine theme with Victor Mono and
ligatures, tabs, line numbers, indent lines and Vim's modes, in a scratch
copy of the sources with its own git repository. Each run waits for clangd
and git first.

With the GPU idle between frames, as in a window, the CPU's part of a
frame is a quarter to a third smaller, and a key reaches the GPU 30 to 40
µs sooner. Back to back, where the GPU sets the pace, there is nothing to
win: with everything on Metal is 2% slower there, and typing back to back
with completion popups costs 7% more CPU a frame; both are left as they
are measured, not explained. The p99 of a key press with everything on is
worse in the medians but the runs overlap (252 to 582 µs against 238 to
520 µs); clangd works in the background through them.

The first version allowed two frames under way, as SDL_GPU does for a
window, and offscreen that cost 35% of the GPU-bound frame rate (11,560
against 17,830): the CPU waited to hear that a frame was done before
making the next, and the GPU waited for the CPU. SDL_GPU puts no limit on
frames offscreen. Three offscreen (two for a window, where the display
sets the pace) put it back.

**Startup.** The shaders are compiled ahead of time with Xcode's Metal
compiler (`shaders/build.sh` makes `.metallib` files, which the build
embeds), and the six pipelines are kept in `~/Library/Caches/blit` (a
Metal binary archive). Making the GPU device then takes 4.7 to 5.2 ms
(shaders 0.05 ms, pipelines 0.7 to 0.9 ms), against 6.7 ms with SDL_GPU.
From source, as before Xcode was there, the shaders took 0.45 ms.

The first run after the shaders change, with nothing cached anywhere,
compiles the pipelines: 91 ms. The first version of the cache made that
110 ms, because each missing pipeline was compiled twice, once to use and
once for the cache; now startup compiles each once, and they are compiled
for the cache on another thread (waited for at exit, so that it is
written). SDL_GPU, the first time after a rebuild, took 635 ms.

**In the same round,** settings moved into `init.lua` (there is no TOML
settings file any more). Every benchmark run with settings now has a Lua
state, which costs 0.37 ms at startup and nothing per key or frame
(Round 18). Blame is shown only when turned on, so git is no longer asked
for a blame of every file opened.

The release binary is 2.23 MB (2.26 before): SDL's GPU calls are gone from
it, the Metal side is small, and the compiled shader libraries are a
little larger than their source.

### Round 20: the evaluation's findings, all of them

A review of the whole program for slow paths, waste and what newer C++
and more threads could do, then each finding fixed and measured against
the build from before it (`before19`), old and new turn about.

**Vim on large files.** Searching went through the document 64 KB at a
time, copying each window out first, and an `\c` search looked for both
cases of the first letter with two `memchr` calls started again at every
candidate, which is quadratic when one case is rare. Now the pieces of the
document are searched where they lie, a match that runs from one piece
into the next is copied out alone, the byte looked for is the pattern's
rarest (the finder's way, now shared), each case has its own `memchr` that
goes on from where it was, and past the first 8 MB the rest is split into
16 MB parts for every core, nearest first. Typing a search (incsearch)
looked through the whole file at every letter; a pattern that has only
grown can only be found at or after where the shorter one was, so it goes
on from there, and from nowhere if the shorter one was not found. `{` and
`}` asked the tree for every line's start and end; now they go from
newline to newline with `memchr` in place. `:s` copied out every line of
its range to look at it; now the matches are found first and only the
lines with one are changed, each with one edit from its first match to its
last, all in one undo step (and `\n` in the pattern now takes a line's
end, as in Vim). `]]` and `[[` gave up after 200,000 lines and now look as
far as the file goes.

| 1 GB log file (100 MB code for the last) | Before | After |
|---|---:|---:|
| `}` from the top | 1,435 ms | 70 ms |
| `{` from the bottom | 1,421 ms | 67 ms |
| `/` typed, a word that is not there, then Enter | 232 ms | 8 ms |
| `?` the same, from the bottom | 278 ms | 4 ms |
| `n` three times more | 89 ms | 16 ms |
| `/word\c` (either case) | 9,123 ms | 23 ms |
| `:%s/word/x/`, not there | 1,849 ms | 29 ms |
| `:%s/word/x/`, not there, 100 MB | 678 ms | 1 ms |

Each key also guessed the file's indentation again (for the indent lines)
when the file had 256 lines or fewer, which made a key in a short file six
times dearer than in a long one: 20,000 keys typed into a 236-line file
took 23 to 36 ms of key handling, now 2 to 3. The guess is now made again
only when the file gains or loses lines. A register's text is shared by
the registers that hold it (`""`, `"1` and the rest moving along) instead
of copied into each, and a put of one copy takes the register's text as it
is.

**Finding.** The folder is read on up to 16 threads at once (each taking
the next folder waiting; what each holds is kept in its own place and the
list is put together in the same order as before). A search for text reads
the first 8 KB of a file and stops there if it is not text, instead of
reading all of it first. Typing onto the end of a search only looks among
what the shorter one found: names among the names that matched, text in
the files that had a line (for ten seconds after the files were all read,
since others may change). The threads that read files went from 8 to as
many as there are cores (16 at most).

| Xcode's `Platforms` folder, 112,000 files | Before | After |
|---|---:|---:|
| Read the folder | 469 ms | 185 to 210 ms |
| Search all of it for a word that is not there | 888 ms | 699 ms |
| … typed a letter at a time, each search waited for | each letter a whole search | 921 ms for all six |
| The SDK's headers (37,757 files): read the folder | 236 ms | 95 ms |

**Typing while things move.** The loop read input, then waited for the
display's next image, then drew. A key that came during the wait was in
the frame after. Now it waits first, takes in what came meanwhile, then
draws. A new benchmark (`--bench motionkeys`) draws frames back to back as
while scrolling, with keys typed by another thread at moments of their
own, and times each key to its frame being handed to the GPU, both ways in
turns: p50 25 ms before, 8.6 ms after (mean 25 → 8.7 ms, 60 Hz display).

**Smaller things.** Script marks are kept by line and only those in view
are drawn. Completions narrow among what matched the shorter word, the
matching no longer asks the locale for each letter's case, and a list of
over 2,000 that is replaced is freed on a thread of its own. `set_lines`
changes only the bytes that differ (a formatter that sets every line
touches only what it changed). Diagnostics in view are found by binary
search. The gutter, when only the cursor's line changed, recolours two
numbers instead of making all of them again. The side panel compares rows
by node, not by path. Git's status, branches and latest commits are asked
for by three programs at once (45 → 30 ms in Homebrew's repository). A
program a script runs producing output wakes the editor once per run of
output instead of once per read. Worker threads say how soon their work
is wanted (macOS quality of service): the finder, syntax and the side
panel are "user initiated", git, measuring and building grammars
"utility". The language server's thread was too at first, and the A/B
below caught it: with everything on, a key's handling went from 1.5 to
2.5 µs. Instruments' Time Profiler (`xctrace`, Xcode's command-line
recorder) put a tenth of the main thread's samples in `write`, under
`Lsp::send`: the one byte that wakes the language server's thread after
every edit, which costs that much more when the thread woken has a lower
quality of service. That thread keeps the drawing thread's again
(1.47 to 1.64 µs a key, against 1.49 to 1.67 before the round). The Metal side keeps one render pass
description and does not give a draw the uniforms the draw before it had.

**A fix.** Git, script programs and grammar builds were started without
`POSIX_SPAWN_CLOEXEC_DEFAULT`, so a program started on one thread could
keep the pipes of another being started at the same moment on another, and
the reader of those pipes would wait for it to end: a script running
`tail -f` could have held up git. They now get nothing but their own three
streams, as the language servers already did. (A test lists `/dev/fd` in a
program run while blit has a pipe open; it fails with the old code.)

**Building.** `common.hpp` included `<chrono>` for its clock (a third of a
second of parsing in every file); it reads the clock with
`clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` now, 8 ns a call. (The first
try, `CLOCK_MONOTONIC`, moves in whole microseconds on macOS and takes
16 ns: the benchmarks' p50 of a key's handling read 0 or 1 µs until the
second evaluation round noticed.) The parallel search's threads are started in a file
of their own, so that `vim.cpp` does without `<thread>`. `applua.cpp`, the
last file to finish, is two files (the API apart from what runs it). A
clean release build went from 8.75 to 7.4 to 7.6 s, and one after touching
`vim.cpp` from 4.75 to 4.5 s. `-mcpu=native` gives `apple-m1` on macOS
(GCC cannot tell the chip); the build now names the chip (`apple-m5`
here), which measured the same (54 micro-benchmarks, geometric mean
0.996).

**Looked at and left.** A precompiled header: the compile time of a file
is four fifths optimization (LTO's early passes), and each target would
first build its own header, which comes before everything else. One
snapshot of the pieces shared by syntax, git and the language server: at
4 KB a piece a 13 MB file's list is 51 KB, a few microseconds to copy.
Starting the GPU on a thread during `SDL_Init`: making it takes 2 ms with
the pipelines cached, against 80 ms of `SDL_Init`. Skipping the cursor's
glide for a one-cell move while typing changes the look and was not asked
for. Batching `git cat-file` for the first marks of many files at once.

**The whole program, old against new** (turn about, 2560x1600; the
window benchmarks in a real window at 60 Hz). This sitting's machine was
slower all round than Round 19's (CPU per frame paced at 160 was 18 µs
then and is 70 here for both builds), so only the columns side by side
compare.

| Benchmark settings (4 runs each) | Before | After |
|---|---:|---:|
| Back to back, frames a second | 18,180 | 18,073 |
| CPU per frame, paced at 160 a second | 74.0 µs | 70.5 µs |
| A key's handling, typing back to back | 0.2 µs | 0.2 µs |
| A key press to its frame handed to the GPU, p50 | 236 µs | 233 µs |
| CPU per frame in a window | 133 µs (106 to 146) | 146 µs (126 to 148) |

| Everything on (8 runs each, two sittings; typing 4, the second) | Before | After |
|---|---:|---:|
| Back to back, frames a second | 17,079 | 16,827 |
| CPU per frame, paced at 160 a second | 96.0 µs | 94.5 µs |
| A key's handling, typing with completions | 1.6 µs | 1.5 µs |
| A key press to its frame handed to the GPU, p50 | 260 µs | 249 µs |
| … p99 | 532 µs | 522 µs |
| CPU per frame in a window | 167 µs (134 to 186) | 183 µs (156 to 200) |

The back-to-back rates overlap (16,547 to 17,663 against 16,718 to
17,595). CPU per frame in a window reads 9% higher in both settings, but
the ranges overlap and the first of the two everything-on sittings had it
the other way round (167 against 158 µs); it is not explained yet, and is
the first thing the next round looks at with Instruments. A key while
something moves is the change that shows: 25 ms to 8.6 ms (above).

### Round 21: a second review, with Instruments

Four reviewers (Claude Sonnet agents) went through the program again, this
time with Xcode's own profilers from the command line (`xctrace`):
Time Profiler (some runs sampling every 100 µs), Metal System Trace,
System Trace, App Launch, File Activity, Allocations and Leaks, and
`leaks`, `heap`, `vmmap`, `footprint`, `malloc_history` besides. One each
on the frame, a key, the threads off the main one, and startup, memory and
large files. Of what they found, four things change what a user meets;
they are done. The rest is listed after them with its numbers.

**A key on the screen 24 ms sooner.** The frames reviewer saw, once, a key
reach the screen in 10 to 15 ms where it usually took 34 to 35, with two
images of the window in use instead of three. Measured here with its probe
(which hooks Metal to note when each frame is handed over and when it is
on the screen), 100 single key presses with pauses between them, six runs
each way, turn about:

| | 3 images | 2 images | 2, and 3 while the view scrolls |
|---|---:|---:|---:|
| A key: handed over to on the screen, p50 of each run | 38.5 to 41.1 ms | 13.8 to 18.4 ms | 13.5 to 18.9 ms |
| Scrolling in a window: late frames in each run (of about 430) | 0, 0, 0, 0, 0, 0 | 118, 36, 0, 0, 149, 0 | 0, 0, 0, 0, 0, 0 |

Two images alone cost smoothness, as one frame in flight did before (see
"Tried and dropped"); switching between them costs nothing measured.
`maximumDrawableCount` is set before each frame: three while the view is
scrolling, two otherwise (`App::choose_images`).

**Operations on many lines.** `:%s`, `>`, `<` and `J` made one edit, and
one undo record, for each line: millions for a whole large file, each a
walk down the tree, and the undo history held about 250 bytes each for
good. Past 2,000 lines they now make as few edits as it takes: `>`, `<` and
`J` rewrite the lines a megabyte at a time, and `:s` joins the edits of
lines close together (up to 4 KB of unchanged text between them, a
megabyte in all), so that a few matches far apart still copy nothing
between them. Finding where each matched line ends had also started a
walk down the tree for each; it is one pass through the text now. Below
2,000 lines each line keeps its own edit, so that marks and the like on a
line stay on it. A test does each operation on 6,000 lines (CRLF lines,
blank and white-space lines, a line of 1.5 MB) both ways, all at once and
in pieces small enough for an edit a line, and compares the text and where
the cursor ends up.

| (each the time of a scripted run, less one without it) | Before | After |
|---|---:|---:|
| `ggVG>` on 100 MB (3.6 M lines) | 710 ms | 40 ms |
| `ggVGJ` on 100 MB | 2.9 to 3.1 s | 44 ms |
| `:%s/e/E/g` on 100 MB | 1.18 s | 137 ms |
| … then `u` and Ctrl-R | 1.66 s in all | 130 ms in all |
| … its peak memory | 1.2 GB | 458 MB |
| `:%s/INFO/WARN/` on the 1 GB log (9.1 M lines) | 3.57 s | 407 ms |
| … its peak memory | 5.26 GB | 2.73 GB |

**Backspace in a search for text.** Typing onto a search looked only in
the files the shorter one had found (Round 20), but only the last search
was kept, so each Backspace read every file again: about 700 ms each in
Xcode's `Platforms` folder (112,000 files), as the threads reviewer timed
them. Now each search typed onto the one before is kept (16 at most), and
one that has been typed back to is searched in what it found:

| Text search, a word not there: typed to six letters, then Backspace to two | |
|---|---:|
| `Platforms` (112,000 files): the four searches after Backspace | 25 ms in all |
| The SDK's headers (37,757 files) | 15 ms in all |

**21 MB handed back.** Compiling a highlight query, tree-sitter asks for a
4.4 MB table for a moment, five at once while the query is compiled in
parts. macOS's malloc keeps memory of that size, once freed, as it is: 21
MB the program held for the rest of its life, a quarter of its footprint
when idle. Blocks of 1 MB and more now come from `mmap` and go back with
`munmap` (`ts_set_allocator`). Idle with a C++ file coloured (no language
server or git): 48.6 MB before, 27.4 MB after. Setting up and parsing take
what they did (15 ms; 441 ms for the 6.6 MB file).

**The clock.** Round 20's `now_ns()` moved in whole microseconds (see
there); it is `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` now.

**Found and left, with what was measured:**

- *A key's own handling.* With everything on, 1.5 µs: half of it caches
  gone cold since the last frame. Building the messages to the language
  server (`snprintf`, strings copied and appended to) is a quarter: a
  patch took a key from 1.41 to 1.07 µs, the same bytes sent. In Normal
  mode the same line is looked up about seven times a key; remembering the
  last two answers took a motion from 0.85 to 0.52 µs, but would have the
  buffer's reading functions write. Neither is a difference anyone can
  see: a key takes 15 to 40 ms to reach the screen.
- *The wake-up `write()`* to the language server's thread, about 0.5 µs a
  key: kqueue, semaphores and futex-style waits cost the same (it is the
  waking of a sleeping thread).
- *Syntax.* After each edit the colours of the lines in view are ready in
  about 0.6 ms. The worker then colours 400 lines either side again, 73% of
  its time (1.8 ms a key on `vim.cpp`), mostly to confirm what it sent: a
  core's time while typing, not a wait. A 13 MB file takes 22 to 25 ms to
  reparse after a keystroke anywhere in it, which is tree-sitter's own cost
  for a file whose top has 16,950 items.
- *Git's marks* hash every line again after an edit: 5.6 ms for a 13 MB
  file on git's thread, 55 µs for a usual one.
- *The finder's threads* are held up by the system, not by blit: 87% of
  their time is in the kernel, nine tenths of it in `open()`, most of that
  the sandbox checking each path, and threads spinning on a kernel lock.
  Sixteen threads search 3.1 times as fast as one; `open` and `close`
  alone are 12.6 µs a file.
- *The frame.* A fifth of the CPU's part is inside Apple's Metal driver
  making the frame's one render pass (4.4 µs warm, 16.4 µs after a pause);
  nine ways of setting it up changed nothing. The GPU's part at 2560x1600 is
  49 µs, limited by pixels drawn; drawing only what changed would cut it,
  but in a window the GPU is busy 0.7% of the time. Holding an image of the
  window while idle would take 20 µs off a key; caching the language
  server's popups and Vim's bottom line, 2 µs off a frame.
- *Startup* is where Round 3 left it. App Launch: 10.7 ms before `main`
  (5 ms of it fixing up SDL's 22 frameworks). Of `SDL_Init`'s 65 to 98 ms,
  29 are CPU on the main thread (AppKit, 5 to 6 of it loading the writing
  tools' menu), the rest waiting on other processes. Opening a window is 25
  to 30 ms. blit's own part: 5 ms before the window, 2 for the GPU, 3 for
  the first frame.
- *Memory.* No leaks in four sessions. Idle with everything on, 69 to 82 MB
  before the 21 MB above; most of a window's memory is its images. A tree
  of tree-sitter's takes 31 bytes for each byte of the file: 400 MB for a
  13 MB file. A file of 1 GB not yet in the page cache shows its first
  frame after 250 ms instead of 115, counting its lines off the disk.
- *Huge yanks* copy the text: 130 ms a gigabyte.
- Scrolling allocates nothing of blit's per frame; a key, 10 allocations.
- The 9% higher CPU per frame in a window that Round 20 could not explain
  is noise: in 24 more pairs the new build was slower in about half, and
  the one high run was all `-[CAMetalLayer nextDrawable]`.

### Round 22: a status bar

A row along the bottom of the window: the branch and how far it is ahead
and behind, this file's lines changed since the last commit, the files
changed, the file's path, the language server's errors and warnings, the
cursor's place and selection, the indentation, the line endings, the
language and its server (`src/appstatus.cpp`). Its cells are made again
only when a few numbers that stand for what it shows change; git's status
is read for it only then. Hunks for the file are now asked of git when the
bar is on even if the line numbers are off.

What it costs, the same build with the bar on and off, everything on (4
runs each, turn about):

| | Off | On |
|---|---:|---:|
| Scrolling back to back: CPU per frame, p50 | 52.6 µs | 52.8 µs |
| … frames a second (the GPU's limit) | 17,530 | 17,320 |
| Typing: the frame's layout (the bar made again at every key) | 0.67 µs | 1.86 µs |
| A key press after a pause, to its frame handed to the GPU, p50 | 261 µs | 281 µs |

The key press after a pause meets cold caches: the bar's dozen strings and
the glyph lookups for its 80 or so characters cost it 20 µs there, against
about 1 µs typing back to back. Next to the 15 ms from a frame handed over
to it on the screen, it is not worth making the bar in parts.

Relative line numbers are now drawn only with Vim's modes on (without them
there are no counts for `j` and `k`).

### Round 23: a start screen

Started with no file, blit shows its name, what can be done first (each
with a key, and the arrows, Enter and the mouse), up to nine of the files
and folders opened lately, and the working folder and settings file
(`src/appstart.cpp`). The list is read once at start, and only by a run
with a window of its own; its cells are made again only when the
selection, the mouse, the size or the list change.

Startup with the screen against without it (a real window, 3 runs each):
the first frame done at 104.8, 107.0 and 114.9 ms against 104.5, 105.5
and 108.0 (the sitting's own spread), its CPU 1.9 to 2.1 ms either way.

### Round 24: another machine's files, over ssh

`ssh://host/path` files and folders: blit starts an agent of its own on
the other machine over one ssh connection (copying it there the first
time) and asks it to read, write, list, walk and search the folder, and
run git and the language server there (`src/remote.cpp`,
`src/agent/blit-agent.cpp`, `src/appremote.cpp`). Nothing on the main
thread waits for the network but saving: a file's tab is there at once
and fills in when its text comes, on a thread of its own.

What it costs the editor when no file is remote: the same build with it
compiled out (`-DBLIT_NO_REMOTE`, where every "is it remote?" is a
constant false and the worker thread is not started) against it compiled
in, turn about, 4 runs each:

| | Out | In |
|---|---:|---:|
| Scrolling back to back, frames a second (the GPU's limit) | 17,839 | 17,836 |
| … everything on | 16,791 | 16,846 |
| Scrolling at 160 Hz: CPU per frame, p50 | 78.1 µs | 79.9 µs |
| … everything on | 99.2 µs | 100.7 µs |
| Typing: the frame's CPU, p50 | 50.0 µs | 49.9 µs |
| A key press after a pause, to its frame handed to the GPU, p50 | 404 µs | 424 µs |
| … everything on | 303 µs | 296 µs |
| A paced frame, p50 (10 runs each) | 165 µs | 151 µs |
| Startup, first frame done (a real window, 5 runs each) | 113.2 ms | 112.4 ms |

All within the sitting's spread. The program is bigger: the Release build
went from 2.3 to 8.8 MB with the three agents inside it (Linux x86-64
2.3 MB and ARM64 2.0 MB, static, built with GCC 16 in Docker; this Mac's
own 1.2 MB). They are read only when one is copied to a machine, so
startup does not see them.

What going through the agent costs, against the same work here: with
`tests/fake-ssh.sh` in place of ssh (the "other machine" is this one, in a
scratch home), so this is the agent's and the pipes' own cost, the floor
under a real link. Whole runs, from starting blit to the answer, median of
5:

| | Here | Through the agent |
|---|---:|---:|
| Opening a 660 KB file | 198 ms | 243 ms (318 the first time: the agent copied) |
| Every file's name under the SDK's `usr/include` (Cmd+P) | 138 ms | 177 ms |
| A word in none of its files, searched for in all of them | 158 ms | 200 ms |

The 40 to 45 ms is making the connection (ssh, a login shell there to
learn its `PATH`, starting the agent); the walk and the search take the
same time there as here, and only names and the lines found cross the
connection.

### Round 25: profiling, and ssh's questions in a box

The side panel's Profile tab records a program with perf (Linux) or
xctrace (macOS), here or on another machine through blit's agent, and
opens the profile in an editor tab: a flame graph over the functions by
self time (`src/profile.*`, `src/appprofile.cpp`). The profile shown
labels the code it has times for. The flame graph is one box whose lit
cells each name one of seven tones (`Popup::tones`, bits 21-23 of a cell),
which grows the uniforms each draw is given from 832 to 960 bytes. ssh's
questions (a password, a passphrase, a new host's key) are asked in
blit's box through `SSH_ASKPASS`, which is blit itself.

What it costs the editor with no profile open: the build before this round
(`main`) against this one, turn about, 4 runs each:

| | Before | After |
|---|---:|---:|
| Scrolling back to back, frames a second (the GPU's limit) | 15,586 | 15,474 |
| … everything on | 17,507 | 17,155 |
| Scrolling at 160 Hz: CPU per frame, p50 | 84.2 µs | 75.1 µs |
| … everything on | 101.8 µs | 109.4 µs |
| Typing: the frame's CPU, p50 | 50.7 µs | 50.8 µs |
| A key press after a pause, to its frame handed to the GPU, p50 | 417 µs | 427 µs |
| … everything on | 270 µs | 260 µs |
| A paced frame, p50 (14 runs each, the new build first) | 141 µs | 118 µs |

All within the sitting's spread. The paced frame is bimodal on this
machine (about 100 µs, or about 620 µs when it lands on a slow core): in
a first run, 8 each and the old build first, the new one landed on the
slow mode more often (313 against 117 µs); run again, 14 each and the new
build first, neither did.

Recording `tests/profile/spin.c`: xctrace takes about 4 s more than the
program's one second (starting Instruments' recorder, saving and exporting
the trace); perf on the Arch machine (Ryzen 9 9950X3D) 5 to 20 ms more
than its half second, and `perf script` 7 to 18 ms. Reading either's
output into a profile (the test fixtures, about 200 KB each) takes 0.4 ms
for perf's and 0.7 ms for xctrace's.

### Round 26: a terminal

Terminal tabs (`src/term.*`, `src/pty.*`, `src/appterm.cpp`): an
xterm-compatible screen fed by a shell under a pseudo-terminal, here or
through blit's agent. The program's output is read and parsed on a thread
of the tab's own; the main thread only draws the screen when it has changed.

Reading what programs write (the parser and the screen alone, a 200 by 50
screen, fed 64 KB at a time):

| | First version | Rows moved whole, plain text written straight |
|---|---:|---:|
| `bench/data/code-100m.txt` (105 MB of code) | 11 MB/s | 137 MB/s |
| `bench/data/utf8-16m.txt` (17 MB of UTF-8 text) | | 206 MB/s |

The first version scrolled by copying every cell of the screen up a row
(160 KB a line at 200 by 50) and copied each line into the history; now a
screen is its rows, scrolling moves them, and a row scrolled off goes into
the history whole, the oldest one making the new blank row once 10,000 are
kept. A run of plain ASCII is written without going through the parser
(127 to 137 MB/s).

From the shell to the screen: `cat` of that 105 MB file in a terminal tab
takes about 0.9 s here and 1.0 s through the agent (the stand-in ssh: no
network), the time to the next prompt.

What it costs the editor with no terminal open: the build before (`main`,
with profiling) against this one, turn about, 4 runs each:

| | Before | After |
|---|---:|---:|
| Scrolling back to back, frames a second (the GPU's limit) | 15,974 | 15,617 |
| … everything on | 16,084 | 15,457 |
| Scrolling at 160 Hz: CPU per frame, p50 | 82.2 µs | 81.8 µs |
| … everything on | 107.8 µs | 101.1 µs |
| Typing: the frame's CPU, p50 | 50.2 µs | 50.2 µs |
| A key press after a pause, to its frame handed to the GPU, p50 | 424 µs | 434 µs |
| … everything on | 282 µs | 286 µs |
| A paced frame, p50 (10 runs each, the new build first) | 140 µs | 135 µs |

All within the sitting's spread (the frames a second overlap: 14,918 to
16,272 before, 15,437 to 15,900 after).

### Round 27: pictures

Image files open in a tab of their own, and pictures are drawn in the
Markdown preview (`src/appimage.cpp`, `src/picture_mac.m`,
`shaders/image.frag`). A picture is read on a thread of its own by macOS's
ImageIO (at most 4096 pixels on a side), put on the GPU as a texture of its
own with its mipmaps (made by the GPU, not waited for), and drawn as one
more draw over the popup it belongs to: the same full-screen triangle cut
to the picture, sampling its texture.

What it costs with no picture on show: the build before (`main` at
5f3c7d5) against this one, turn about, 4 runs each, offscreen at
2560x1600, medians:

| | Before | After |
|---|---:|---:|
| Scrolling back to back, frames a second (the GPU's limit) | 17,240 | 19,315 |
| … tab bar, line numbers, indent lines | 17,852 | 18,016 |
| Scrolling: CPU per frame, p50 | 35.8 µs | 35.9 µs |
| … tab bar, line numbers, indent lines | 39.7 µs | 39.7 µs |
| Typing: frames a second | 19,573 | 19,207 |
| Typing: CPU per frame, p50 | 35.2 µs | 35.3 µs |

All within the sitting's spread (the "before" scrolling runs went from
16,303 to 19,276).

Scrolling a file in one split with a 3000x2000 picture in the other (3
runs each, against the same file beside a second text file):

| | Frames a second | CPU per frame, p50 | Layout |
|---|---:|---:|---:|
| A text file beside it | 16,460 to 17,626 | 31.0 µs | 0.67 µs |
| The picture, blended over its tab's box | 13,381 to 13,473 | 48.7 µs | 1.36 µs |
| The picture, written straight | 16,456 to 17,102 | 33.1 µs | 0.69 µs |

Blended, the picture cost 15 µs of GPU time a frame (the CPU waited for
it: 37.5 µs of "begin" against 22): the GPU drew the tab's box under the
picture and then the picture over it. A picture with nothing see-through
(or with the checkerboard behind it, which makes every pixel solid) now
goes through a second pipeline that writes it straight, and Apple's GPU
leaves out the box's pixels under it: the frame costs what a text view in
that split costs. Blending stays for pictures with see-through pixels in
the Markdown preview, and for any not on whole pixels. The tab's bottom
row was also made again every frame (its text built and hashed: 0.7 µs);
now only when what it says changes. The 2 µs of CPU left is the one more
draw, with its texture switched in and the atlas switched back.

From opening to the picture on screen: a 3000x2000 PNG (9.9 MB) about 30 ms
more than a run that reads nothing (181 to 201 ms against 152 to 163 for
the whole run), the same picture as a JPEG (538 KB) about 10 ms.

### Round 28: a review of all of it, and realistic work

PDFs in a tab of their own (`src/apppdf.cpp`: pages drawn by CoreGraphics on
a thread as they come into view). A 300-page text-heavy PDF at 2560x1600:
its first pages on screen about 40 ms after the program's own start (179 to
224 ms in all, against about 150 for a run that opens nothing), and each
page turn, with its pages drawn, about 5 ms.

Then the whole program was reviewed by five reviewers (one model, Sonnet,
each with one part and no notes but the README) and what they found that
held up was fixed: crashes and hangs (the language server reading a closed
document's text, closing a terminal whose shell ignores SIGHUP, the
preview's code blocks a few columns wide, quotes nested thousands deep,
a recursive user command), Vim's (macros that stop at a failing command,
`cw` on a word's last letter, the exclusive-motion rule, `3.`, appending
registers, Replace mode's Backspace), files (fsync before a save's rename,
a rename that differed only in case overwriting another file), and the
project generator (Python `src/` layouts, uv's dependencies, CMake's
reserved target names). The full suite before (5f3c7d5 plus PDFs, a build
of `main` at 536b074) and after was run, and anything that moved more than
the sitting's spread was run again turn about with the two builds side by
side, 3 runs each:

| | Before | After |
|---|---:|---:|
| Typing in README.md (1,600 lines) with its preview open: the frame's CPU, p50 | 1,826 µs | 53 µs |
| … p99 | 2,129 µs | 1,089 µs |
| … the layout's part, p50 | 1,807 µs | 24 µs |
| Idle (3 s after the window has settled): frames drawn | 49, 50 | 0, 0 |
| … CPU | 0.53%, 0.74% | 0.02%, 0.02% |
| A key press, to its frame handed to the GPU, p50 | 185, 194 µs | 187, 191 µs |
| Save, code-1m.txt (1 MB) | 0.38 to 0.42 ms | 0.80 to 0.98 ms |
| Save, utf8-16m.txt (16 MB) | 1.9 to 2.1 ms | 2.7 to 3.4 ms |
| Paste, code-100m.txt | 27.2 to 28.7 MB/ms | 26.9 to 28.8 MB/ms (one run 20) |

- **The Markdown preview is drawn on a thread of its own.** Each key typed
  in a file with its preview open parsed and laid out the whole document in
  the frame (1.8 ms for this README). The file's text is now copied and
  handed to a worker (the newest for each preview; one not started yet is
  dropped), and the rows are taken when they come. The p99 left is the
  frame that takes them and puts the rows on show into cells. The first
  drawing of a preview is still done at once, so that it opens drawn.
- **Idle is idle again.** The scroll thumb fades away 1.2 s after the view
  last moved, and a split with nothing to scroll ran that fade (0.3 s of
  frames) for a thumb that is never drawn, at startup and for each new
  split. It has no thumb now. The idle benchmark also waits until nothing
  is moving before it measures, as Round 3 meant it to.
- **A save waits for the disk.** The new file is flushed (`fsync`) before
  it takes the old one's place, and its folder after, so that a crash
  cannot leave an empty file where the old one was: 0.5 to 1 ms more a
  save, kept.
- **The finder reads the folder again only when it may have changed**
  (opened more than 2 s after the last reading, the window back in front,
  or the file tree having seen a change), in the background, searching the
  list it has meanwhile. 14,551 files take 32 ms to read; a Cmd+P after
  another no longer waits for that, and switching between names and text
  never does.

Everything else that moved between the two full runs (taken hours apart)
moved the same way in code that was not touched, and did not move when
run turn about.

### Round 29: a UI/UX overhaul

Three audits (a visual one from screenshots, one of keys, flows and wording,
and a map of where each box's looks were set) became one set of rules in
`src/look.hpp` (spacing in points, the inks of each role, keys as macOS
writes them, hint lines, ellipses), and five implementers applied them to
the pickers, the side panel, the status bar and chrome, the start screen
and New project, and the keys and messages. What it costs, turn about with
the build before it (`main` at 84189c9), 2560x1600:

| | Before | After |
|---|---:|---:|
| Scrolling, all chrome on: frames a second (this sitting switched between two states) | 15,917 to 17,011; 9,053 to 9,144 | 16,893 to 17,122; 8,772 to 10,101 |
| … layout, p50 (in the faster state) | 0.79 to 0.83 µs | 0.88 µs |
| Typing: layout, p50, without / with the side panel | 18.0 to 19.5 / 16.6 to 20.0 µs | 18.2 to 20.8 / 17.3 to 20.2 µs |
| Idle in a window: frames, CPU | 0, 0.03% | 0, 0.03% |
| A key press to its frame handed to the GPU, p50 | 139 µs | 142 µs |

Nothing measurable. What could have cost: the status bar's mode is a lit
run with a tone of its own (its tones are worked out each frame from the
theme: a few multiplications), the side panel's tab strip measures its
names when the tab or the width changes, the picker's field has a tone,
and keys are turned into symbols once each time a box opens, not each
frame. Every box still rebuilds its cells only when its cache key changes.

### Round 30: sessions

A folder opened to work in comes back as it was left: its tabs, splits,
cursors and views, the side panel and the Build tab's task
(`src/appsession.cpp`, one small text file per folder in `sessions` beside
`recent`). Nothing happens per frame: the start screen looks up each folder's
tab count when its cells are made again, from a map. Files are written on a
thread of their own; the counts Recent shows are read on a thread started
before SDL is, while the window is made, and taken in before the first
frame. `blit DIR` used to open the folder as an empty file; now it opens it
to work in.

Startup, turn about with `main` at 88c9ca6 (a real window, 9 launches of
each, the first frame done, median; the same launch spread from 141 to
515 ms this sitting, so the medians are within it;
`bench/results/r30-sessions-startup.jsonl`):

| | Before | After |
|---|---:|---:|
| A file given (`code-1m.txt`) | 157.6 ms | 156.0 ms |
| The start screen, with 30 in Recent (10 folders with sessions of 7 tabs) | 155.4 ms | 148.8 ms |
| `blit DIR`, bringing back 7 C++ files in two splits | (an empty file) | 174.3 ms |

What the feature itself takes on the main thread, timed around each call
in a copy of the build with timers in it (not committed):

| | |
|---|---:|
| Started with a file: the sessions' state made | 4 µs |
| Start screen: the thread that reads Recent's counts started | 17 to 45 µs |
| … its answer taken in at the first frame (it was long done) | 10 to 25 µs |
| `blit DIR` with 7 files: read, placed, cursors and views set | 0.8 to 1.5 ms |
| A session written (the text made; the file is the thread's) | 16 to 64 µs |
| Another folder opened, from one with 7 tabs to one with 7 | 3.3 to 6.1 ms |

Found and left: of a folder switch, 3.5 to 4.6 ms is closing the 7 files
it leaves, about 0.5 ms each, which closing any coloured tab costs (each
document's syntax worker is joined and its parser, tree and query freed on
the main thread). Once per switch, and under a frame at 120 Hz; moving the
freeing to a thread would touch every way a tab closes, so it was not done
here.

Afterwards, with the terminals' split along the bottom on `main` (Round
32): a restore puts that split away whole and back below the split with
the keyboard, so its shells keep running where they were. That is two moves
of one split in the tree when a folder is opened, nothing at startup or per
frame, and it was not timed on its own.

### Round 31: build problems

A build's output feeds the Problems tab (`src/buildscan.*`): GCC's,
Clang's, CMake's, make's and Python's problems are read from the Run
button's terminal as they come, on the thread that already reads it, and
handed to the main thread with the screen's own wake-up; F8 goes through
them, the status bar counts them, the Run button says how many, and
Cmd+click opens a `path:line` in any terminal. Nothing is parsed per frame:
the main thread takes a batch when one comes (each merged in after the
others of its file) and the panel, the status bar and the Run button
rebuild their cells only when the count of changes (`build_prob_gen_`)
moves.

What the reading thread does with each 64 KB (`blit_bench run FILE term`:
a 200 by 50 screen; a 32 MB build log made up in the bench, GCC's coloured
errors with their source lines, Ninja's progress written over itself, make
going into folders, CMake's and Python's; two runs):

| | The screen alone | The screen and the scan | The scan alone |
|---|---:|---:|---:|
| `code-100m.txt` | 69.5, 69.3 MB/s | 59.3, 58.1 MB/s | 346, 341 MB/s |
| The build log (5,000 problems kept: the most a run keeps) | 146, 148 MB/s | 121, 119 MB/s | 740, 735 MB/s |

Instruments' Time Profiler on the same (`code-1m.txt`): the screen 80% of
the time, the scan 15% (7% its walk over the bytes, 4% the lines). A
build that prints 10 MB costs the scan 15 to 30 ms on a thread nobody waits
for.

Then the build before (`main` at 88c9ca6, built from a clean copy) against
this one, turn about, offscreen at 2560x1600. The machine was busy with
other builds (load average 8 to 15), so the frame rates are not this
machine's quiet ones:

| | Before | After |
|---|---:|---:|
| A 32 MB build log through the Run button's terminal, start to end, median of 8 | 0.564 s (0.697 in a first sitting) | 0.550 s (0.684) |
| The screen alone (`Term`, which now marks rows a line wrapped from), median of 6: code / the build log | 69.2 / 182 MB/s | 69.5 / 190 MB/s |
| Typing: the frame's layout, p50, median of 6 | 2.15 µs | 2.00 µs |
| Scrolling with the tab bar, line numbers and indent lines: layout, p50 | 1.31 µs | 1.27 µs |
| … frames a second (GPU-bound, and loaded) | 9,611 | 10,728 |

Nothing measurable. A first version read the build's counts for the status
bar from behind the build's pointer (`buildui_`), which the status bar does
at every key; that run put typing's layout 0.2 to 0.3 µs above the build
before, so the counts are `App` members now (Round 17's lesson), and the
difference went. Under this load that is a lead, not a finding. The Run
button's cells, made every frame before, are now made only when what it
shows changes.

### Round 32: the terminals' split

Ctrl+` shows a terminal in the split along the bottom that the build's
output uses, and with the keyboard there hides the split again
(`src/appterm.cpp`: the split is taken out of the arrangement whole and kept,
its programs still running, and put back as it was); Ctrl+Shift+` opens
another there. Closing a terminal whose program still runs asks first, and
so does quitting, with the count. Nothing of it runs per frame: the split's
state is looked at only on Ctrl+`, a build's run, closing a terminal tab and
quitting. `term_sync`, which runs when a terminal has written something,
looks at the hidden split too, with no allocation (an index past the
splits on show).

Turn about with `main` at 6276f5c, 6 runs of each, alternating, offscreen at
2560x1600 (the medians of the runs; each run's own p50 for the frame's CPU
was 35.0 to 37.1 µs on both sides):

| | Before | After |
|---|---:|---:|
| Typing: layout, p50 | 1.25 µs | 1.23 µs |
| … the frame's CPU, p50 / p99 | 36.6 / 117.5 µs | 36.6 / 116.8 µs |
| Keys (moving): layout, p50 | 1.21 µs | 1.21 µs |
| … the frame's CPU, p50 / p99 | 35.3 / 108.0 µs | 35.2 / 102.6 µs |
| Scrolling: layout, p50 | 0.38 µs | 0.38 µs |
| … frames a second (the GPU's limit) | 18,358 | 18,198 |

Nothing measurable. What asking costs, once a close or a quit: whether a
terminal's shell is running something is the terminal's foreground process
group (`tcgetpgrp` on its side of the pseudo-terminal: 5.6 to 6.0 µs here),
and the program's name its arguments (`sysctl(KERN_PROCARGS2)`: 7.4 to 7.6
µs).

### Round 33: regular expressions, and replacing (one of three changes made side by side: see Round 34)

Vim's `/`, `?`, `n`, `*`, `gn` and `:s`, the highlighting, and the find
boxes take Vim's regular expressions, on an engine of blit's own
(`src/regex.cpp`) that reads the document where it lies, a piece at a time.
A pattern compiles to a Pike VM (every way a match could go, run at once:
the text's length times the program's at worst, never exponential). A
pattern with nothing special in it is not run there: it is text, looked for
as before. A pattern that stays on a line and has three bytes or more of
plain text every match must contain has that looked for with `memchr`
(the finder's way), and only the lines it is on go through the VM. Any
other one-line pattern is passed over the text by a DFA built as it goes
(a state for each set of the VM's places that turns up, ASCII by a table,
the rest by a map), line by line, and the VM runs only on a line the DFA
says has a match, to say where.

On one thread, 105 MB of code (the medians of three runs; `blit_bench run
FILE find`):

| Pattern | Way | Speed |
|---|---|---:|
| `qzjx\d\+` (plain text in it, not there) | prefilter | 21.6 GB/s |
| `[A-Z]\{3}[0-9]\{4}` (no plain text, not there) | DFA | 0.55 GB/s |
| `qzjx\|vkqz\|zzqq` (an alternation, not there) | DFA | 0.55 GB/s |
| `\<\d\+\>` (1,052,787 lines have one) | DFA, then the VM on each | 0.29 GB/s |
| `tensor shape` as a phrase (replacing's search) | text | 7.4 GB/s |

Vim's `/` (`Vim::find`, on as many threads as it takes past 8 MB): the
class and the alternation, not there, 46 ms in the 105 MB, 182 ms in the
1 GB log. Searching for text as it stands is unchanged, turn about with
`main` at 4ce9b19, built against each library (best of 5, three rounds):

| `Vim::find`, a word not there | Before | After |
|---|---:|---:|
| 1 GB log, rare letters, forward / back | 4.88 to 5.09 / 4.94 to 5.11 ms | 4.92 to 4.99 / 4.88 to 5.03 ms |
| 105 MB code, common letters, forward | 9.81 to 10.76 ms | 9.98 to 12.01 ms |
| every `return` in 105 MB (421,115) | 17.2 to 18.4 ms | 17.4 to 18.1 ms |

Search in files' word grep is the same code, and compiles to the same
instructions (`find_text` and `find_term` are byte for byte those of
`main`): best of 7, 2.45 to 2.55 GB/s before and 2.51 to 2.53 after for a
word that is not there in the 105 MB. The frames (offscreen at 2560x1600,
three runs each, alternating, with and without Vim's modes): typing, keys
and scrolling 35.0 to 38.6 µs of CPU a frame at p50 on both sides, the
layout 0.33 to 1.33 µs on both. The matches are lit from a cache keyed on
the text, the view and the pattern, as before; a regular expression is
looked for line by line, a long line as far as 8 KB, and only when that
key changes.

The machine was shared with other work during these runs; the numbers
compare runs taken together.

### Round 34: three changes made side by side, merged

Search and replace (Round 33), remote work that never waits on the main
thread, and characters made of several code points (with the Markdown
preview's code blocks coloured) were each made in a copy of `main` at
4ce9b19 by an agent of its own, measured there turn about against that
build, then merged (seven files changed by two of them merged with no
conflict). The merged build against 4ce9b19, turn about, medians of 5 to 6:

| | Before | After |
|---|---:|---:|
| Fill, code-100m.txt, sequential / random | 1.270 / 1.560 µs | 1.319 / 1.578 µs |
| Fill, utf8-16m.txt, sequential / random | 6.907 / 7.151 µs | 7.238 / 7.428 µs |
| Search in files, a common word, code-100m.txt | 2.31 GB/s | 2.24 GB/s |
| … letters in no line, utf8-16m.txt | 26.6 GB/s | 29.4 GB/s |
| Typing a character, code-100m.txt | 22.0 ns | 22.4 ns |
| Scrolling at 2560x1600, code-100m.txt: frames a second / CPU p50 / fill p50 | 16,374 to 17,397 / 30.9 µs / 1.25 µs | 15,881 to 17,349 / 30.6 µs / 1.25 µs |
| … utf8-16m.txt | 16,860 to 17,439 / 30.2 µs / 4.62 to 4.88 µs | 16,640 to 17,240 / 30.1 µs / 4.62 to 4.83 µs |

- **Fill is 1 to 4% slower on ASCII text, 4 to 5% on UTF-8, in
  `blit_bench`; the same in a frame.** Text that is not ASCII now has its
  width looked up in a table of Unicode's (two loads above U+0300), and a
  character that may join the one before (a mark, a joiner, an emoji)
  takes a slow path; ASCII takes none of it. What is left on ASCII text is
  where the code lies: keeping the row's last place only when a mark could
  follow, which takes work out, measured +12% on code-100m.txt, and a
  build with the old `grid.cpp` and everything else new measured −0.5%.
  So it was not kept, and the 0.02 to 0.05 µs a frame stands.
- **Remote work, the main thread's time** (the agent that made it, against
  4ce9b19 with the same timing verbs added): a save on a host 200 ms away
  408 ms → 0.006 ms; a keystroke into a folder there 408 ms → 0.005 ms;
  a file made or renamed in the tree there 205 to 604 ms → 0.004 to 0.012
  ms. A remote terminal running `yes`: 72 → 619 MB in 8 s before, 91 MB
  flat after (and with an agent from before, which cannot be paused: blit
  stops reading at 4 MB instead).
- The line-lookup benchmark, in code none of the three touched, moved 12%
  between the two builds in the same sitting: the size of this machine's
  noise for numbers this small.

### Tried and dropped

- **Smaller or larger pieces** (1, 2, 8 KB instead of 4 KB): line lookups
  move by 10 to 25%, tree memory by 2 to 4x the other way. Random access is
  dominated by cache misses on the file itself. Kept 4 KB.
- **Uploading only part of the grid**: the cost of an upload is fixed
  overhead, not bytes. No gain.
- **Raising the main thread's priority**: no change to keypress latency.
- **Static FreeType**: 0.6 ms off startup for a fragile link line. Not worth
  it.
- **AppKit launch options** (disabling dictation and character palette menu
  items): no measurable change to `SDL_Init`.
- **One frame in flight instead of two**: more late frames (2 to 4%).
- **Reading and writing JSON numbers with `std::from_chars` and
  `std::to_chars`**: the floating-point versions bring 150 KB of tables into
  the program, and nearly every number a language server sends is a small
  whole one. Whole numbers are read by hand, the rest by `strtod`: same
  speed (1.4 GB/s), smaller binary (1.20 → 1.05 MB).
- **Background indexing for clangd by default**: it parses the whole
  project on every core for an editor that shows one file. Off unless the
  settings ask for it.
- **Reading each visible line's indentation from the text** for the indent
  lines: a tree lookup per row, 4.9 µs a frame against 0.35 µs for reading
  it off the cells the frame has just made, which already say where each
  line's text starts. Reading the text is kept only for a view scrolled
  sideways, where the cells do not show the start of the line.
- **Shading only the part of a view the text is in** (blank pixels right
  of the lines left out): a test that drew a fifth of each text view's
  width ran slower, not faster, GPU-bound (13,700 to 14,300 frames a second
  against 17,400 to 18,100 at 2560x1600). The frame is not limited by the
  shader's work per pixel. (Round 19's review.)
- **A DFA row of 256 entries, so that a byte costs one load and one
  comparison** (a line's end, a "\r" and a byte past ASCII marked in it,
  for the slow path): slower, 470 against 580 MB/s for a class or an
  alternation over 105 MB, three runs each, alternating. The row of 128
  with the three tests before it stays.
- **Keeping the highlight query in slices for good**: each slice needs its
  own walk over the tree, so four slices make a pass 2.4x slower (2.3 →
  5.7 ms). They are only used for the first moments.

## Reading these numbers

- The machine's state moves results by 10 to 20% between sittings (the
  `SDL_Init` time alone ranged from 62 to 79 ms across runs). Compare runs
  taken together, and trust back-to-back A/B tests over cross-run deltas.
  The startup improvement that belongs to blit is the 5 ms before `main` and
  the 30 ms for large files; the rest of the difference between rounds is
  that variation.
- Offscreen frame rates are GPU bound at these resolutions. "CPU per frame"
  in offscreen runs includes time blocked on the GPU queue once it is full.
- A frame drawn right after the thread wakes costs about four times what the
  same frame costs back to back (about 100 µs against 25 to 45 µs), because
  the CPU has gone idle in between. Real scrolling and typing see the larger
  number. It is 1.6% of a 160 Hz frame.
- Only Metal has been run. Up to Round 18 it was reached through SDL_GPU;
  from Round 19 blit has its own Metal backend, and the Vulkan side is not
  written yet.
