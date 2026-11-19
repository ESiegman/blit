#!/bin/sh
# Language servers through the editor, against the predictable test server
# (tests/fake_lsp.cpp): several at once, every tab's file open on one, the
# Problems tab across files, references, the other places a name leads to
# (implementations, type definitions, declarations, the header or source,
# the file's outline, the project's symbols, its other uses in the file),
# rename, formatting, code actions, inlay hints and the servers' colours,
# by their keys and Vim's.
#
#   tests/lsp.sh path/to/blit            run the cases
#   tests/lsp.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/lsp.sh path/to/blit [--print]}
print=${2:-}
fake="$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp"
[ -x "$fake" ] || { echo "lsp.sh: no $fake"; exit 1; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
P="$dir/proj"
servers="blit.opt.lsp.servers.fakec = { command = { \"$fake\", \"--name\", \"c\" }, languages = { \"cpp\" }, roots = { \".root\" } }
blit.opt.lsp.servers.fakepy = { command = { \"$fake\", \"--utf8\", \"--name\", \"py\" }, languages = { \"python\" }, roots = { \".root\" } }
blit.opt.git.enabled = false
blit.opt.editor.restore = false"
printf '%s\nblit.opt.editor.modal = false\n' "$servers" >"$dir/init.lua"
printf '%s\nblit.opt.editor.modal = true\n' "$servers" >"$dir/vim.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.lsp.format_on_save = true\n' "$servers" >"$dir/save.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.lsp.inlay_hints = true\n' "$servers" >"$dir/hints.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.lsp.semantic_colors = false\n' "$servers" >"$dir/plain.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.syntax.styles["lsp.parameter"] = "#ff0000 italic"\n' "$servers" >"$dir/themed.lua"
printf 'blit.opt.lsp.servers.clangd = { command = { "%s", "--name", "c" }, languages = { "cpp" }, roots = { ".root" } }\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' "$fake" >"$dir/clangd.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.lsp.highlight_uses = false\n' "$servers" >"$dir/nouses.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.lsp.reference_counts = false\n' "$servers" >"$dir/nolens.lua"
printf 'blit.opt.lsp.servers.fakec = { command = { "%s", "--workspace", "--name", "c" }, languages = { "cpp" }, roots = { ".root" } }\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' "$fake" >"$dir/pull.lua"
printf 'blit.opt.lsp.servers.fakec = { command = { "%s", "--late", "300", "--name", "c" }, languages = { "cpp" }, roots = { ".root" } }\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' "$fake" >"$dir/late.lua"
printf 'blit.opt.lsp.servers.fakec = { command = { "%s", "--hints-once", "--name", "c" }, languages = { "cpp" }, roots = { ".root" } }\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\nblit.opt.lsp.inlay_hints = true\n' "$fake" >"$dir/hintsonce.lua"
printf 'blit.opt.lsp.servers.fakec = { command = { "%s", "--close-input", "200", "--name", "c" }, languages = { "cpp" }, roots = { ".root" } }\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' "$fake" >"$dir/closing.lua"
export BLIT_RECENT_FILE="$dir/recent"
init=
fresh() {
    rm -rf "$P"
    mkdir -p "$P"
    : >"$P/.root"
    printf 'int shared = 1; // ERROR\ncall(1,   2, 3);   \nWARN shared  \n' >"$P/a.cpp"
    printf 'shared = 2  # HINT\n' >"$P/b.py"
    printf 'int c; // WARN\n' >"$P/c.cpp"
    printf 'shared in d, not open\n' >"$P/d.cpp"
    printf 'int x;\ncall(1, 2, 3);\nauto width = p_size;\nWidget w; MAX_SIZE\n' >"$P/h.cpp"
    printf 'int one = 1;\nint two = one;\nint three = one + two;\n' >"$P/e.cpp"
    mkdir -p "$P/src" "$P/include"
    printf 'struct Widget {\n    int size;\n    void draw() {\n    }\n};\nint total(int n) {\n    return n;\n}\n' >"$P/src/app.cpp"
    printf 'struct Widget;\n' >"$P/include/app.hpp"
    printf 'int util() {\n    return 1;\n}\n' >"$P/src/util.cpp"
    printf '// beside\n' >"$P/src/util.hpp"
    printf '// included\n' >"$P/include/util.hpp"
    printf '/* \303\251\303\251 */ int wide(int n) {\n}\n' >"$P/w.cpp"
    printf 'struct Shape {\n};\nstruct Circle : public Shape {\n};\nstruct Square : Shape {\n};\nint leaf(int n) {\n    return n;\n}\nint middle(int n) {\n    return leaf(n) + leaf(1);\n}\nint top() {\n    return middle(2);\n}\n' >"$P/calls.cpp"
}

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT [FILE]: the last line the script prints; lines that say what
# was done (blit: ...) are kept apart, the last of them in $said. with
# SETTINGS SCRIPT [FILE]: the same with other settings.
run() {
    fresh
    out=$(BLIT_INIT="${init:-$dir/init.lua}" "$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$P;frame;lspsettle;$1" "${2:-$P/a.cpp}" 2>&1 >/dev/null)
    got=$(printf '%s\n' "$out" | grep -v '^blit: ' | tail -1)
    said=$(printf '%s\n' "$out" | grep '^blit: ' | tail -1)
}
with() {
    init="$dir/$1.lua"
    shift
    run "$@"
    init=
}

# Several servers at once: each language's, each tab's file open on it.
run "lspdump"
check "a server for the file in front" 'fakec proj 1 | a.cpp+ | front followed 3'
run "open:$P/b.py;open:$P/c.cpp;lspsettle;lspdump"
check "C++ and Python together: two servers, every tab's file open" 'fakec proj 2; fakepy proj 1 | a.cpp+ b.py+ c.cpp+ | front followed 2'
run "lspidle:150;open:$P/b.py;open:$P/c.cpp;lspsettle;open:$P/b.py;key:cmd+W;lspdump;wait:400;lspdump"
check "the last Python tab closed: its server stops a while after" 'fakec proj 2 | a.cpp+ c.cpp+ | front followed 2'
run "open:$P/b.py;open:$P/c.cpp;lspsettle;open:$P/a.cpp;key:cmd+W;wait:100;lspdump"
check "a tab closed: its file is closed on its server, which stays for the others" 'fakec proj 1; fakepy proj 1 | b.py+ c.cpp+ | front followed 2'

# Problems for the whole project, by file; F8 walks them across files.
run "open:$P/b.py;open:$P/c.cpp;lspsettle;problemsdump"
check "the Problems tab: every open file's problems, by file" 'button: ▶  | file 7 | file a.cpp | >H 1 text:46aa6896d35a2ff7 | E 1 found ERROR | W 3 found WARN | file b.py | H 1 text:3c0b00648b72acf9 | H 1 found HINT | file c.cpp | H 1 text:23d998acb00f2736 | W 1 found WARN'
run "open:$P/b.py;open:$P/c.cpp;lspsettle;open:$P/a.cpp;key:F8;key:F8;problemsdump"
check "F8 goes to the next problem in the file" 'button: ▶  | file 7 | file a.cpp | H 1 text:46aa6896d35a2ff7 | E 1 found ERROR | >W 3 found WARN | file b.py | H 1 text:3c0b00648b72acf9 | H 1 found HINT | file c.cpp | H 1 text:23d998acb00f2736 | W 1 found WARN'
run "open:$P/b.py;open:$P/c.cpp;lspsettle;open:$P/a.cpp;key:F8;key:F8;key:F8;dump"
check "and on into the next file's" '>[a.cpp b.py* c.cpp]'
run "open:$P/b.py;open:$P/c.cpp;lspsettle;open:$P/a.cpp;key:shift+F8;dump"
check "Shift+F8 goes back, round the start into the last file" '>[a.cpp b.py c.cpp*]'
with pull "problemsdump"
check "a server that reports the whole project's problems: files that are not open too" 'button: ▶  | file 5 | file a.cpp | >H 1 text:46aa6896d35a2ff7 | E 1 found ERROR | W 3 found WARN | file c.cpp (not open) | H 1 text:23d998acb00f2736 | W 1 found WARN'
with pull "key:F8;key:F8;key:F8;key:F8;dump"
check "F8 opens a file that is not open, at its problem" '>[a.cpp c.cpp*]'

# Find references: a list like Search in files; Enter goes there; F4 steps on.
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:200;finddump"
check "Shift+F12 lists the references: file, line, text" 'pick [] 4 : >a.cpp:1  int shared = 1; // ERROR a.cpp:3  WARN shared   b.py:1  shared = 2  # HINT d.cpp:1  shared in d, not open'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:200;key:Down;key:Return;vimdump"
check "Enter goes to the one chosen" 'NORMAL |  | 3:6 | WARN shared  '
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:200;key:Return;key:F4;vimdump"
check "F4 goes to the next" 'NORMAL |  | 3:6 | WARN shared  '
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:200;key:Return;key:F4;key:F4;key:F4;dump"
got="$got / $said"
check "on into other files, open or not" '>[a.cpp b.py d.cpp*] / blit: Reference 4 of 4'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:200;key:Return;key:F4;key:F4;key:F4;key:F4;key:shift+F4;dump"
got="$got / $said"
check "round the end, and Shift+F4 back" '>[a.cpp b.py d.cpp*] / blit: Reference 4 of 4'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:200;key:Return;key:cmd+P;frame;key:Escape;key:F4;dump"
got="$got / $said"
search_keys='Ctrl+Shift+F'  # (as the bottom line writes Search in files' keys: symbols on a Mac)
[ "$(uname -s)" = Darwin ] && search_keys='⇧⌘F'
check "another list made since: F4 is Search in files' again" ">[a.cpp*] / blit: No search in files yet: $search_keys"
with vim "text:w;text:g;text:r;wait:200;finddump"
check "Vim's gr lists them too" 'pick [] 4 : >a.cpp:1  int shared = 1; // ERROR a.cpp:3  WARN shared   b.py:1  shared = 2  # HINT d.cpp:1  shared in d, not open'
run "key:Down;key:End;key:shift+F12;wait:200;finddump"
got="$got / $said"
check "nothing there: it says so" 'closed / blit: No references found'

# Where else a name leads: one answer is gone to, several are listed.
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:cmd+F12;wait:200;vimdump"
check "Cmd+F12 goes to the one implementation" 'NORMAL |  | 3:6 | WARN shared  '
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:cmd+F12;wait:200;finddump" "$P/e.cpp"
check "several are listed: file, line, text" 'pick [] 2 : >e.cpp:2  int two = one; e.cpp:3  int three = one + two;'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:cmd+F12;wait:200;key:Down;key:Return;vimdump" "$P/e.cpp"
check "Enter goes to the one chosen" 'NORMAL |  | 3:13 | int three = one + two;'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:cmd+shift+F12;wait:200;vimdump" "$P/e.cpp"
check "Cmd+Shift+F12, the type definition (a Location, not a list)" 'NORMAL |  | 3:13 | int three = one + two;'
with vim "text:G;text:w;text:w;text:w;text:gD;wait:200;vimdump" "$P/e.cpp"
check "Vim's gD, the declaration" 'NORMAL |  | 1:5 | int one = 1;'
with vim "text:w;text:gy;wait:200;vimdump" "$P/e.cpp"
check "gy, the type definition" 'NORMAL |  | 3:13 | int three = one + two;'
with vim "text:G;text:w;text:w;text:w;text:gd;wait:200;vimdump" "$P/e.cpp"
check "gd is still the definition" 'NORMAL |  | 1:5 | int one = 1;'
run "key:Down;key:End;key:cmd+F12;wait:200;finddump"
got="$got / $said"
check "nothing there: it says so" 'closed / blit: No implementation found'

# The header for a source and the other way round: beside it, then up the
# folders; clangd, which knows from what includes what, asked first.
run "key:alt+F12;wait:100;dump" "$P/src/app.cpp"
check "Option+F12 finds the header up the folders" '>[app.cpp app.hpp*]'
run "key:alt+F12;wait:100;key:alt+F12;wait:100;dump" "$P/src/app.cpp"
check "and back, to the tab it came from" '>[app.cpp* app.hpp]'
run "key:alt+F12;wait:100;vimdump" "$P/src/util.cpp"
check "one beside it first" 'NORMAL |  | 1:1 | // beside'
with clangd "key:alt+F12;wait:200;vimdump" "$P/src/util.cpp"
check "clangd's answer where there is one" 'NORMAL |  | 1:1 | // included'
run "key:alt+F12;wait:100;dump" "$P/e.cpp"
got="$got / $said"
check "none: it says so" '>[e.cpp*] / blit: No header or source found for e.cpp'

# The file's outline, and the project's symbols as they are typed.
run "key:cmd+alt+O;wait:200;finddump" "$P/src/app.cpp"
check "Cmd+Option+O lists the file's symbols, nested" 'pick [] 3 : >Widget  · struct   draw()  · method total()  · function'
run "key:cmd+alt+O;wait:200;text:dr;key:Return;vimdump" "$P/src/app.cpp"
check "typing narrows them; Enter goes there" 'NORMAL |  | 3:10 |     void draw() {'
run "key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:cmd+alt+O;wait:200;key:Up;key:Up;frame;key:Escape;vimdump" "$P/src/app.cpp"
check "Escape leaves the cursor where it was" 'NORMAL |  | 7:1 |     return n;'
with vim "text:gO;wait:200;finddump" "$P/src/app.cpp"
check "Vim's gO" 'pick [] 3 : >Widget  · struct   draw()  · method total()  · function'
run "key:cmd+alt+P;frame;text:tot;frame;wait:200;frame;finddump" "$P/src/app.cpp"
check "Cmd+Option+P asks the server as it is typed" 'pick [tot] 1 : >total  · function  · src/app.cpp:6'
run "key:cmd+alt+P;frame;text:tot;frame;wait:200;frame;key:Return;vimdump" "$P/src/app.cpp"
check "Enter goes there" 'NORMAL |  | 6:5 | int total(int n) {'
run "key:cmd+alt+P;frame;text:wide;frame;wait:200;frame;key:Return;vimdump" "$P/src/app.cpp"
check "in a file not open, past letters UTF-16 counts otherwise: to its name" 'NORMAL |  | 1:16 | /* éé */ int wide(int n) {'
run "key:cmd+K;frame;text:draw;frame;wait:200;frame;finddump" "$P/src/app.cpp"
check "Search everywhere has them too" 'everywhere [draw] 1 : Symbols: >draw (method in Widget  · src/app.cpp:3)'
run "key:cmd+K;frame;text:draw;frame;wait:200;frame;key:Return;vimdump" "$P/src/app.cpp"
check "and goes to one" 'NORMAL |  | 3:10 |     void draw() {'

# Call and type hierarchies, a level at a time: the first level opened,
# Right opens a row's, Left closes it (or goes up), Enter goes there.
on_middle="key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Right;key:Right;key:Right;key:Right;key:Right"
run "$on_middle;command:Show the calls to this function;hierdump" "$P/calls.cpp"
check "the calls to a function" '▾ middle()  · function  · calls.cpp:10 |   ▸ top()  · function  · calls.cpp:14'
run "$on_middle;command:Show the calls to this function;hierdump;key:Down;key:Right;hierdump" "$P/calls.cpp"
check "Right: the calls to a caller (none)" '▾ middle()  · function  · calls.cpp:10 |   ▾ top()  · function  · calls.cpp:14'
run "$on_middle;command:Show the calls from this function;hierdump" "$P/calls.cpp"
check "the calls from it, each where it is" '▾ middle()  · function  · calls.cpp:10 |   ▸ leaf()  · function  · calls.cpp:7 |   ▸ leaf()  · function  · calls.cpp:7'
run "$on_middle;command:Show the calls from this function;hierdump;key:Left;frame;hierdump" "$P/calls.cpp"
check "Left: a level closed" '▸ middle()  · function  · calls.cpp:10'
run "$on_middle;command:Show the calls to this function;hierdump;key:Down;key:Return;vimdump" "$P/calls.cpp"
check "Enter: to where it calls" 'NORMAL |  | 14:12 |     return middle(2);'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;command:Show the subtypes of this type;hierdump" "$P/calls.cpp"
check "a type's subtypes" '▾ Shape  · struct  · calls.cpp:1 |   ▸ Circle  · struct  · calls.cpp:3 |   ▸ Square  · struct  · calls.cpp:5'
run "key:Down;key:Down;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;command:Show the supertypes of this type;hierdump" "$P/calls.cpp"
check "and supertypes" '▾ Circle  · struct  · calls.cpp:3 |   ▸ Shape  · struct  · calls.cpp:1'

# Peek: the definition in a box under the line, with the keyboard; Esc
# closes it and gives the keyboard back; what is typed there is the file's.
run "vim:3G;vim:fw;command:Peek the definition;peekdump" "$P/e.cpp"
check "a peek: the definition, in a box inside the split, with the keyboard" 'e.cpp:2 | int two = one; | keyboard | inside'
run "vim:3G;vim:fw;command:Peek the definition;peekdump;key:Escape;peekdump;vimdump" "$P/e.cpp"
check "Esc: gone, the keyboard back where it was" 'NORMAL |  | 3:20 | int three = one + two;'
run "vim:3G;vim:fw;command:Peek the definition;peekdump;vim:A // seen<Esc>;key:Escape;vim:2G;vimdump" "$P/e.cpp"
check "typed in the box: the file's" 'NORMAL |  | 2:22 | int two = one; // seen'

# How many references each definition on show has, after its line; a
# click on one lists them.
run "lensdump" "$P/src/app.cpp"
check "each definition's references, counted" '1: 2 references | 3: 1 reference | 6: 1 reference'
with nolens "lensdump" "$P/src/app.cpp"
check "lsp.reference_counts = false: none" 'no counts'

# The other uses of the name under the cursor, a moment after it stops.
uses="key:Right;key:Right;key:Right;key:Right;key:Right;frame;wait:250;frame;wait:100;frame"
run "$uses;usesdump" "$P/e.cpp"
check "the name's uses in the file, marked" 'uses: 1:5-8 2:11-14 3:13-16'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;frame;wait:250;frame;wait:100;frame;usesdump" "$P/e.cpp"
check "with the cursor just after it too" 'uses: 1:5-8 2:11-14 3:13-16'
run "$uses;key:Right;key:Right;key:Right;frame;usesdump" "$P/e.cpp"
check "gone when the cursor leaves the name" 'uses: none'
run "$uses;key:shift+Right;frame;usesdump" "$P/e.cpp"
check "and with a selection" 'uses: none'
run "key:Down;key:Down;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;frame;wait:250;frame;wait:100;frame;usesdump" "$P/h.cpp"
check "none for a name used once" 'uses: none'
with nouses "$uses;usesdump" "$P/e.cpp"
check "none with lsp.highlight_uses off" 'uses: none'

# Rename: where the name is, the new name in the box, what it changes, then the change.
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;finddump"
check "F2 asks for the new name, starting from the old" 'input [shared] 0 :'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;key:ctrl+U;text:common;key:Return;wait:200;finddump"
check "then shows what it would change, file by file" 'pick [] 4 : >Rename to common: 4 edits in 3 files a.cpp  2 edits b.py  1 edit d.cpp  1 edit'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;key:ctrl+U;text:common;key:Return;wait:200;key:Return;lsptext"
got="$got / $said / $(cat "$P/d.cpp")"
check "Enter renames: the open file through its tab, the other on disk" 'int common = 1; // ERROR / call(1,   2, 3);    / WARN common   /  / blit: Renamed: 4 edits in 3 files / common in d, not open'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;key:ctrl+U;text:common;key:Return;wait:200;key:Return;key:cmd+Z;lsptext"
check "one undo takes it back in the open file" 'int shared = 1; // ERROR / call(1,   2, 3);    / WARN shared   / '
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;key:ctrl+U;text:common;key:Return;wait:200;lua:blit.insert('x');key:Return;lsptext"
got="$got / $said / $(cat "$P/d.cpp")"
check "typed in while what it changes is on show: Enter makes none of it" 'int sxhared = 1; // ERROR / call(1,   2, 3);    / WARN shared   /  / blit: Cannot rename: the text changed meanwhile / shared in d, not open'
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;finddump"
got="$got / $said"
check "a name the server will not rename: it says why" 'closed / blit: Cannot rename: ERROR cannot be renamed'
with late "key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;key:ctrl+U;text:common;key:Return;key:Home;text:xx;wait:700;lsptext"
got="$got / $said"
check "typed in while a slow server renames: its edits, about the old text, are not made" 'xxint shared = 1; // ERROR / call(1,   2, 3);    / WARN shared   /  / blit: Cannot rename: the text changed meanwhile'

# Formatting: the file, the lines selected, Vim's gq, and before a save.
# A completion that is a snippet: put in with its tab stops, the first
# (in three places) filled in at once, then the next, then where it ends.
# (The text's end: Cmd+Down on a Mac, Ctrl+End elsewhere.)
case $(uname -s) in Darwin) text_end=cmd+Down ;; *) text_end=ctrl+End ;; esac
run "key:$text_end;text:for_l;key:ctrl+space;wait:200;key:Return;snippetdump"
check "a snippet completed: its stops, the first in three places" '[4:6-4:7,4:13-4:14,4:22-4:23] 4:17-4:18 4:26-4:26'
run "key:$text_end;text:for_l;key:ctrl+space;wait:200;key:Return;text:j;key:Tab;text:10;key:Tab;text:go();lsptext"
check "filled in at each stop" 'int shared = 1; // ERROR / call(1,   2, 3);    / WARN shared   / for (j = 0; j < 10; ++j) {go()}'
run "key:alt+shift+F;wait:200;lsptext"
got="$got / $said"
check "Option+Shift+F formats the file" 'int shared = 1; // ERROR / call(1, 2, 3); / WARN shared /  / blit: Formatted'
run "key:alt+shift+F;wait:200;key:cmd+Z;lsptext"
check "one undo takes it back" 'int shared = 1; // ERROR / call(1,   2, 3);    / WARN shared   / '
run "key:Down;key:Down;key:shift+End;key:alt+shift+F;wait:200;lsptext"
check "with a selection, its lines alone" 'int shared = 1; // ERROR / call(1,   2, 3);    / WARN shared / '
with vim "text:j;text:g;text:q;text:q;wait:200;lsptext"
check "Vim's gqq formats the line" 'int shared = 1; // ERROR / call(1, 2, 3); / WARN shared   / '
with vim "text:g;text:q;text:G;wait:200;lsptext"
check "and gq with a motion, the lines it covers" 'int shared = 1; // ERROR / call(1, 2, 3); / WARN shared / '
with save "text:x;key:cmd+S;wait:100;lsptext"
got="$got / $(tr '\n' '|' <"$P/a.cpp")"
check "with lsp.format_on_save, a save formats first" 'xint shared = 1; // ERROR / call(1, 2, 3); / WARN shared /  / xint shared = 1; // ERROR|call(1, 2, 3);|WARN shared|'

# Code actions: a menu of what the server offers at the cursor.
run "key:Down;key:Down;key:alt+Return;wait:200;finddump"
check "Option+Enter lists the actions here, a fix first" 'pick [] 4 : >Replace WARN with NOTE Make it capitals Add a comment above Not now (it never is)'
run "key:Down;key:Down;key:alt+Return;wait:200;key:Return;lsptext"
check "a fix makes its edit" 'int shared = 1; // ERROR / call(1,   2, 3);    / NOTE shared   / '
run "key:Down;key:Down;key:End;key:Left;key:Left;key:Left;key:Left;key:alt+Return;wait:200;finddump"
check "on a name, without a problem: what can be done to it" 'pick [] 3 : >Make it capitals Add a comment above Not now (it never is)'
run "key:Down;key:Down;key:End;key:Left;key:Left;key:Left;key:Left;key:alt+Return;wait:200;key:Return;wait:200;lsptext"
check "one the server fills in when chosen" 'int shared = 1; // ERROR / call(1,   2, 3);    / WARN SHARED   / '
run "key:Down;key:Down;key:alt+Return;wait:200;key:Down;key:Down;key:Return;wait:300;lsptext"
got="$got / $said"
check "a command, whose edit the server asks for" 'int shared = 1; // ERROR / call(1,   2, 3);    / // fake / WARN shared   /  / blit: Add a comment above'
run "key:Down;key:Down;key:alt+Return;wait:200;key:Down;key:Down;key:Down;key:Return;wait:100;lsptext"
got="$got / $said"
check "one that cannot be used says why" 'int shared = 1; // ERROR / call(1,   2, 3);    / WARN shared   /  / blit: Not now: it never is'
with late "key:Down;key:Down;key:alt+Return;key:End;text:z;wait:700;finddump"
check "typed in while a slow server finds them: none can be used, and each says why" 'pick [] 4 : >Replace WARN with NOTE (the text changed meanwhile) Make it capitals (the text changed meanwhile) Add a comment above (the text changed meanwhile) Not now (it never is)'
with late "key:Down;key:Down;key:End;key:Left;key:Left;key:Left;key:Left;key:alt+Return;wait:700;key:Return;key:Home;text:z;wait:700;lsptext"
got="$got / $said"
check "or while one is filled in: it is not made" 'int shared = 1; // ERROR / call(1,   2, 3);    / zWARN shared   /  / blit: Make it capitals: the text changed meanwhile'

# Inlay hints between the characters (off unless asked for), and the
# servers' colours over tree-sitter's.
with hints "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "inlay hints where the server puts them, and its colours" 'hints: 2:6[first: ] 2:9[second: ] 2:12[third: ] 3:11[: int] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hints "frame;wait:100;frame;key:Down;key:Down;col:7;rowsdump:3" "$P/h.cpp"
check "scrolled sideways into a hint: what is right of the edge of it, the text after it" 'rows | |irst: 1, second: 2, third: 3); |idth = p_size;'
with hints "frame;wait:100;frame;key:Down;key:Down;col:12;rowsdump:3" "$P/h.cpp"
check "and past it: the text moves on a column at a time, the hint's room kept" 'rows | | 1, second: 2, third: 3); |= p_size;'
with hints "key:Down;frame;wait:100;frame;lspextras" "$P/h.cpp"
check "none on the cursor's line" 'hints: 3:11[: int] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hints "frame;wait:100;frame;key:Down;key:Down;key:End;text: + 1;frame;lspextras" "$P/h.cpp"
check "kept in step with an edit until asked again" 'hints: 2:6[first: ] 2:9[second: ] 2:12[third: ] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hintsonce "frame;wait:100;frame;key:Down;key:Down;key:End;text: + 1;frame;wait:400;frame;wait:100;frame;lspextras" "$P/h.cpp"
check "asked again, and answered with an error: the hints there stay" 'hints: 2:6[first: ] 2:9[second: ] 2:12[third: ] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hints "frame;wait:100;frame;key:Up;text:auto p_size = 1;key:Return;frame;wait:400;frame;wait:100;frame;lspextras" "$P/h.cpp"
check "and asked again once the text stands still" 'hints: 1:12[: int] 3:6[first: ] 3:9[second: ] 3:12[third: ] 4:11[: int] | colours: p_size=variable.parameter call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
# With soft wrap: each hint on the row its column is on, and the rows make
# room for them, so that no text is lost. wrapped SCRIPT [LINE]: a long
# line with hints, wrapped in a narrow window; got is that line's rows as
# drawn (the padding before each left out) joined by a space, "too wide"
# after them if any row holds more than the wrap's width, and then the
# wrap's rows (line@column).
long='int y = 0; call(alpha, beta, gamma); then some more words to make this line long enough that it wraps a couple of times in a narrow view; call(1, 2, 3);'
drawn='int y = 0; call(first: alpha, second: beta, third: gamma); then some more words to make this line long enough that it wraps a couple of times in a narrow view; call(first: 1, second: 2, third: 3);'
wrapped() {
    fresh
    printf 'int x;\n%s\nauto width = p_size;\n' "$long" >"$P/wrap.cpp"
    got=$(BLIT_INIT="$dir/hints.lua" "$blit" --offscreen 700x800 --shot "$dir/shot.bmp" --script "workdir:$P;frame;lspsettle;frame;wait:100;frame;command:Toggle soft wrap;frame;$1;wrapdump;rowsdump:16" "$P/wrap.cpp" 2>&1 >/dev/null |
        grep -v '^blit: ' | tail -2 | awk -v want="${2:-2}" '
            /^width / { width = $2 + 0; at = substr($0, index($0, "rows: ") + 6); n = split(at, w, " "); for (i = 1; i <= n; i++) { split(w[i], p, "@"); line[i] = p[1] } }
            /^rows / { m = split($0, r, / [|]/); for (i = 2; i <= m; i++) if (line[i - 1] == want) { s = r[i]; sub(/^ +/, "", s); out = out (out == "" ? "" : " ") s; if (length(s) > width) over = 1 } }
            END { print out (over ? " too wide" : "") " / " at }')
}
wrapped "frame"
rows=${got##* / }
got=${got% / *}
check "soft wrap: each hint on its row, the rows making room (no text lost)" "$drawn"
case $rows in *"2@0 2@"*) ;; *) echo "FAIL: soft wrap with hints: the line does not wrap: $rows"; failed=1 ;; esac
wrapped "key:Down;frame"
check "the cursor's line: no hints drawn, its rows as they were" "$long / $rows"
wrapped "key:End;key:Return;text:int z;frame" 3
below=$(printf '%s' "$rows" | awk '{ for (i = 1; i <= NF; i++) { split($i, p, "@"); n = p[1] + (p[1] >= 2); out = out (i > 1 ? " " : "") n "@" p[2] (i == 1 ? " 2@0" : "") } print out }')
check "a line put in above: the hints move with their text, before they are asked for again" "$drawn / $below"
run "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "no hints unless lsp.inlay_hints is on" 'hints: | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with themed "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "a theme's own colour for a kind of token" 'hints: | colours: call=function p_size=lsp.parameter Widget=type MAX_SIZE=function.macro'
with plain "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "no colours of the server's with lsp.semantic_colors off" 'hints: | colours:'

# A server that closes its input and keeps its output open: its output is
# waited for, with nothing spinning meanwhile.
with closing "wait:500;cpuquiet:1000"
check "a server's input closed, its output open: nothing spins" 'quiet'

[ "$failed" = 0 ] && echo "lsp: all passed"
exit "$failed"
