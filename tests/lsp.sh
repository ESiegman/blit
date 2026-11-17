#!/bin/sh
# Language servers through the editor, against the predictable test server
# (tests/fake_lsp.cpp): several at once, every tab's file open on one, the
# Problems tab across files, references, rename, formatting, code actions,
# inlay hints and the servers' colours, by their keys and Vim's.
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
printf 'blit.opt.lsp.servers.fakec = { command = { "%s", "--workspace", "--name", "c" }, languages = { "cpp" }, roots = { ".root" } }\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' "$fake" >"$dir/pull.lua"
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
run "key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:F2;wait:200;finddump"
got="$got / $said"
check "a name the server will not rename: it says why" 'closed / blit: Cannot rename: ERROR cannot be renamed'

# Formatting: the file, the lines selected, Vim's gq, and before a save.
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

# Inlay hints between the characters (off unless asked for), and the
# servers' colours over tree-sitter's.
with hints "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "inlay hints where the server puts them, and its colours" 'hints: 2:6[first: ] 2:9[second: ] 2:12[third: ] 3:11[: int] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hints "key:Down;frame;wait:100;frame;lspextras" "$P/h.cpp"
check "none on the cursor's line" 'hints: 3:11[: int] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hints "frame;wait:100;frame;key:Down;key:Down;key:End;text: + 1;frame;lspextras" "$P/h.cpp"
check "kept in step with an edit until asked again" 'hints: 2:6[first: ] 2:9[second: ] 2:12[third: ] | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with hints "frame;wait:100;frame;key:Up;text:auto p_size = 1;key:Return;frame;wait:400;frame;wait:100;frame;lspextras" "$P/h.cpp"
check "and asked again once the text stands still" 'hints: 1:12[: int] 3:6[first: ] 3:9[second: ] 3:12[third: ] 4:11[: int] | colours: p_size=variable.parameter call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
run "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "no hints unless lsp.inlay_hints is on" 'hints: | colours: call=function p_size=variable.parameter Widget=type MAX_SIZE=function.macro'
with themed "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "a theme's own colour for a kind of token" 'hints: | colours: call=function p_size=lsp.parameter Widget=type MAX_SIZE=function.macro'
with plain "frame;wait:100;frame;lspextras" "$P/h.cpp"
check "no colours of the server's with lsp.semantic_colors off" 'hints: | colours:'

[ "$failed" = 0 ] && echo "lsp: all passed"
exit "$failed"
