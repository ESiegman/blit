#!/bin/sh
# Editing comforts: lines moved and copied, brackets and quotes closed as
# they are typed, and more carets than one. Needs the C++ grammar for the
# cases in a comment.
#
#   tests/edit.sh path/to/blit            run the cases
#   tests/edit.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/edit.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
base='blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.opt.editor.restore = false'
printf '%s\nblit.opt.editor.modal = false\n' "$base" >"$dir/init.lua"
printf '%s\nblit.opt.editor.modal = true\n' "$base" >"$dir/vim.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.editor.close_pairs = false\n' "$base" >"$dir/off.lua"
snips='blit.snippet("fn", "int ${1:name}(${2:void})\n{\n\t$0\n}", {lang = "cpp", desc = "a function"})
blit.snippet("pair", "${1:a} = ${1:a} + ${2|one,two|};")
blit.snippet("wrap", "(${TM_SELECTED_TEXT:nothing})$0")'
printf '%s\nblit.opt.editor.modal = false\n%s\n' "$base" "$snips" >"$dir/snip.lua"
printf '%s\nblit.opt.editor.modal = true\n%s\n' "$base" "$snips" >"$dir/snipv.lua"
export BLIT_RECENT_FILE="$dir/recent"
init=
fresh() {
    printf 'one\ntwo\nthree\nfour' >"$dir/l.txt"
    printf 'foo one\nfoo two\nbar foo\nend\n' >"$dir/m.txt"
    printf 'int f() {\n    g(x);\n}\n' >"$dir/p.cpp"
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
# run SCRIPT [FILE]: the text after it, its lines joined by " / ", and the
# cursor (vimdump's line:column), joined by " @ ".
run() {
    fresh
    out=$(BLIT_INIT="${init:-$dir/init.lua}" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;$1;frame;lsptext;vimdump" "${2:-$dir/l.txt}" 2>&1 >/dev/null | grep -v '^blit: ' | tail -2)
    got="$(printf '%s\n' "$out" | head -1) @ $(printf '%s\n' "$out" | tail -1 | cut -d'|' -f3 | tr -d ' ')"
}
with() {
    init="$dir/$1.lua"
    shift
    run "$@"
    init=
}
up="keyof:Move the lines up"
down="keyof:Move the lines down"

# Lines moved and copied.
run "$down"
check "Option+Down: the line goes down past the next" 'two / one / three / four @ 2:1'
run "key:Down;$up"
check "Option+Up: up" 'two / one / three / four @ 1:1'
run "key:Down;key:Down;$down"
check "onto the last line, which has no end of its own" 'one / two / four / three @ 4:1'
run "key:Down;key:Down;key:Down;$down"
check "the last line: nowhere to go" 'one / two / three / four @ 4:1'
run "key:Down;key:shift+Down;key:shift+Down;$down"
check "the lines chosen (one the selection ends at the start of is not)" 'one / four / two / three @ 4:6'
run "key:Down;$down;key:cmd+Z"
check "one undo step (the cursor back where it was)" 'one / two / three / four @ 2:1'
run "key:Down;command:Copy the lines down"
check "copied below: the cursor goes with the copy" 'one / two / two / three / four @ 3:1'
run "key:Down;command:Copy the lines up"
check "above: it stays with the lines it was on" 'one / two / two / three / four @ 2:1'
with vim "text:j;text:V;text:j;$up"
check "Vim: Visual mode's lines" 'two / three / one / four @ 2:5'

# Brackets and quotes closed as they are typed.
F="$dir/p.cpp"
run "key:Down;key:End;key:Return;text:h;text:(" "$F"
check "an opening bracket puts its closer after the cursor" 'int f() { /     g(x); / h() / } /  @ 3:3'
run "key:Down;key:End;key:Return;text:h;text:(;text:a;text:)" "$F"
check "the closer typed steps over it" 'int f() { /     g(x); / h(a) / } /  @ 3:5'
run "key:Down;key:End;key:Return;text:h;text:(;key:Backspace" "$F"
check "Backspace between them takes both" 'int f() { /     g(x); / h / } /  @ 3:2'
run "key:Down;key:End;key:Return;text:s = \";text:x;text:\"" "$F"
check "quotes the same way" 'int f() { /     g(x); / s = "x" / } /  @ 3:8'
run "key:Down;key:End;key:Return;text:{;key:Return;text:y" "$F"
check "Enter between braces: an indented line between them" 'int f() { /     g(x); / { /     y / } / } /  @ 4:6'
run "key:Down;key:End;key:Left;key:Left;key:Left;key:shift+Right;text:[" "$F"
check "with a selection: round it" 'int f() { /     g([x]); / } /  @ 2:9'
run "key:Down;key:End;key:Left;key:Left;key:Left;text:(" "$F"
check "not before a word" 'int f() { /     g((x); / } /  @ 2:8'
run "key:Down;key:End;key:Return;text:// it's (fine" "$F"
check "nor a quote after a letter, nor in a comment" 'int f() { /     g(x); / // it'"'"'s (fine / } /  @ 3:14'
run "key:Down;key:End;key:Left;key:Left;text:)" "$F"
check "a closer it did not put in is typed" 'int f() { /     g(x)); / } /  @ 2:9'
with off "key:Down;key:End;key:Return;text:h;text:(" "$F"
check "editor.close_pairs = false" 'int f() { /     g(x); / h( / } /  @ 3:3'
with vim "text:jA;text: q(;text:1;text:);key:Escape" "$F"
check "Vim's Insert mode" 'int f() { /     g(x); q(1) / } /  @ 2:14'

# More carets than one: added on the lines below, at the next match and at
# every match; typing, deleting and moving at each, one undo for all; Esc
# leaves the primary; copied a line each, and pasted a line each where the
# lines are as many as the carets. (And with Vim: commands at each.)
# (The commands by name: off a Mac the line below has no key, and with Vim's
# modes Ctrl+D is Vim's. And the text's ends: Cmd+Up and Cmd+Down on a Mac,
# Ctrl+Home and Ctrl+End elsewhere.)
below="command:Add a caret on the line below"
next="command:Add the next match to the carets"
every="command:Add carets to every match"
case $(uname -s) in Darwin) text_start=cmd+Up ;; *) text_start=ctrl+Home ;; esac
# carets SCRIPT [FILE] [INIT]: the text after it (" / " between lines) and every caret.
carets() {
    fresh
    out=$(BLIT_INIT="${3:-$dir/init.lua}" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;$1;frame;lsptext;caretdump" "${2:-$dir/m.txt}" 2>&1 >/dev/null | grep -v '^blit: ' | tail -2)
    got="$(printf '%s\n' "$out" | head -1) @ $(printf '%s\n' "$out" | tail -1)"
}
carets "$below;$below;key:End;text:!" "$dir/l.txt"
check "carets on the lines below, typed at each" 'one! / two! / three! / four @ *1:5 2:5 3:7'
carets "$next;$next;$next;text:X"
check "the next match, thrice, typed over" 'X one / X two / bar X / end /  @ 1:2 2:2 *3:6'
carets "$every;key:Backspace;key:Right;text:_"
check "every match, deleted, then moved and typed at each (past a line's end: the next line)" ' _one /  _two / bar  / _end /  @ *1:3 2:3 4:2'
carets "$every;key:Backspace;key:cmd+z;key:Escape"
check "one undo for all, the primary's selection back; Esc leaves one" 'foo one / foo two / bar foo / end /  @ *1:4-1:1'
carets "$below;$below;key:End;key:shift+word+Left;key:cmd+c;key:Escape;key:$text_start;$below;$below;key:cmd+v"
check "copied a line each, pasted a line each" 'onefoo one / twofoo two / foobar foo / end /  @ *1:4 2:4 3:4'
carets "$below;$below;text:A!;key:Escape" "$dir/l.txt" "$dir/vim.lua"
check "Vim: a command at each" 'one! / two! / three! / four @ *1:4 2:4 3:6'
carets "$next;$next;$next;text:cX;key:Escape" "$dir/m.txt" "$dir/vim.lua"
check "Vim: the matches as Visual mode's, changed at each" 'X one / X two / bar X / end /  @ 1:1 2:1 *3:5'
carets "$below;$below;text:dw;text:u" "$dir/m.txt" "$dir/vim.lua"
check "Vim: undone once, for all" 'foo one / foo two / bar foo / end /  @ *1:1'
# Snippets of one's own: Tab after the trigger puts one in, indented, and
# goes from stop to stop; a stop in two places takes what is typed at both;
# a choice is chosen; one wraps what is selected (from the list of them);
# one for C++ is not put in elsewhere; undo, or Esc, ends it.
printf 'x\n' >"$dir/s0.cpp"
snip() { cp "$dir/s0.cpp" "$dir/s.cpp"; carets "$1" "$dir/s.cpp" "$dir/${2:-snip}.lua"; }
snip "key:End;key:Return;text:fn;key:Tab;text:add;key:Tab;key:Tab;text:return 1"
check "Tab after a trigger: the snippet, filled in from stop to stop" 'x / int add(void) / { / 	return 1 / } /  @ *4:13'
snip "text:A;key:Return;text:fn;key:Tab;text:add;key:Tab;text:int a;key:Tab;text:return a;key:Escape" snipv
check "with Vim, in Insert mode" 'x / int add(int a) / { / 	return a / } /  @ *4:12'
snip "key:End;key:Return;text:pair;key:Tab;text:k;key:Tab;key:Down;key:Return"
check "a stop in two places, then a choice" 'x / k = k + two; /  @ *2:12'
snip "key:shift+End;command:Insert a snippet…;text:wrap;key:Return;text:!"
check "one wrapping what is selected, from the list" '(x)! /  @ *1:5'
snip "key:End;key:Return;text:fn;key:Tab;key:cmd+z;key:Tab"
check "undo ends it (Tab then puts it in again, not on to its next stop)" 'x / int name(void) / { / 	 / } /  @ *2:9-2:5'
cp "$dir/s0.cpp" "$dir/s.txt"
carets "key:End;key:Return;text:afn;key:Tab" "$dir/s.txt" "$dir/snip.lua"
check "a trigger inside a longer word, or for another language: a tab" 'x / afn	 /  @ *2:5'

if [ "$(uname -s)" = Darwin ]; then  # (the cells' size is the font's: the Mac's, here)
    carets "click:70,50;release;click:70,76,alt;release;click:70,76,alt;release;click:70,63,alt;drag:90,89,alt;release;text:Q"
    check "Alt+click: a caret, gone again; Alt+drag: a column" 'fooQ one / fooQo / barQo / endQ /  @ 1:5 2:5 3:5 *4:5'
fi

# Local history: the text on disk kept at the first save (frames drawn
# while typing before it, as a window would), then each save; the versions
# listed, each tried out as a diff against now, one put back.
export BLIT_HISTORY_DIR="$dir/data"
printf 'one\ntwo\n' >"$dir/hist.txt"
BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;text:X;frame;frame;save;text:Y;frame;save;frame" "$dir/hist.txt" >/dev/null 2>&1
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;historydump" "$dir/hist.txt" 2>&1 >/dev/null | tail -1)
check "local history: the text before the first save, and each save" "sss"
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;command:Show the file's local history;frame;key:Down;frame;lsptext;key:Down;frame;key:Return;frame;lsptext;dump:tabs" "$dir/hist.txt" 2>&1 >/dev/null | grep -v '^blit: ' | tail -3 | tr '\n' '#')
check "tried out: the diff against now; chosen: put back, the tab of diffs gone" "@@ -1 +1 @@ / -Xone / +XYone /  two#one / two / #tabs: >[hist.txt*]#"
# Where the history cannot be kept (a file in the way of its folder): the
# file is saved all the same, and blit goes on.
echo "in the way" >"$dir/nohist"
export BLIT_HISTORY_DIR="$dir/nohist"
printf 'one\n' >"$dir/hist2.txt"
BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;text:X;save;frame" "$dir/hist2.txt" >/dev/null 2>&1
got="$? $(cat "$dir/hist2.txt")"
check "local history that cannot be kept: saved all the same" "0 Xone"
unset BLIT_HISTORY_DIR

# Spelling: the words of comments and strings (not names, addresses or
# capitals), of Markdown's text (not its code); Option+Enter on one, its
# suggestions. Where there is no English checker, skipped.
printf '// This functon adds the numbrs, see http://example.com/teh_page\nint totl_count = 0;  // HTTP and getValue are fine\nconst char* s = "a mispelled string";\n' >"$dir/sp.c"
printf '# Notes\n\nSome recieved text with `codde` in it.\n' >"$dir/sp.md"
printf '%s\nblit.opt.editor.modal = true\nblit.opt.editor.spell_check = false\n' "$base" >"$dir/nospell.lua"
got=$(BLIT_INIT="$dir/vim.lua" "$blit" --offscreen 1400x600 --shot "$dir/shot.bmp" --script "frame;spelldump" "$dir/sp.c" 2>&1 >/dev/null | tail -1)
if [ "$got" = "no checker" ]; then
    echo "spelling skipped: no English checker here"
else
    check "spelling: comments and strings, not names, addresses or capitals" "functon numbrs mispelled"
    got=$(BLIT_INIT="$dir/vim.lua" "$blit" --offscreen 1400x600 --shot "$dir/shot.bmp" --script "frame;spelldump" "$dir/sp.md" 2>&1 >/dev/null | tail -1)
    check "Markdown: its text, not its code" "recieved"
    got=$(BLIT_INIT="$dir/vim.lua" "$blit" --offscreen 1400x600 --shot "$dir/shot.bmp" --script "frame;spelldump;vim:ff;key:alt+Return;frame;finddump;key:Return;frame;spelldump" "$dir/sp.c" 2>&1 >/dev/null | tail -2 | sed -e 's/pick \[\] [0-9]* : >\([a-z]*\) .*Add.*/first \1, and Add/' | tr '\n' '#')
    check "Option+Enter: its suggestions (the dictionary's last); chosen, it is right" "first function, and Add#numbrs mispelled#"
    got=$(BLIT_INIT="$dir/nospell.lua" "$blit" --offscreen 1400x600 --shot "$dir/shot.bmp" --script "frame;spelldump" "$dir/sp.c" 2>&1 >/dev/null | tail -1)
    check "editor.spell_check = false: none" "off"
fi

# Soft wrap: a Markdown file wraps (each break after a space), a C file
# not until Option+Z; gj and gk go by rows along the row, j by lines; a
# click on a wrapped row lands in its line; typing on keeps the map.
long="This paragraph goes on for a good while so that it has to wrap onto the rows under it when the view is narrower than it is, which it will be here, more than once."
printf '# Title\n\n%s\n\nShort.\n' "$long" >"$dir/wrap.md"
printf 'int x; // %s\nint y;\n' "$long" >"$dir/wrap.c"
wr() { BLIT_INIT="$dir/${3:-vim}.lua" "$blit" --offscreen 900x500 --shot "$dir/shot.bmp" --script "frame;$1" "$2" 2>&1 >/dev/null | grep -v '^blit: '; }
got=$(wr "wrapdump" "$dir/wrap.md" | tail -1)
rows=$(printf '%s' "$got" | sed -n 's/.*, \([0-9]*\) rows.*/\1/p')
case $got in "width "*) [ "${rows:-0}" -gt 5 ] && got=wraps ;; esac
check "a Markdown file wraps by default" "wraps"
breaks=$(wr "wrapdump" "$dir/wrap.md" | tail -1 | tr ' ' '\n' | grep '^3@' | grep -v '^3@0$' | sed 's/3@//')
ok=wrapped
for b in $breaks; do ch=$(printf '%s' "$long" | cut -c"$b"); [ "$ch" = " " ] || ok="broke at $b: [$ch]"; done
[ -n "$breaks" ] || ok="no breaks"
got=$ok
check "each break after a space" "wrapped"
got=$(wr "wrapdump" "$dir/wrap.c" | tail -1)
check "a C file does not" "off"
got=$(wr "key:alt+z;wrapdump;key:alt+z;wrapdump" "$dir/wrap.c" | tail -2 | sed -e 's/^width .*/on/' | tr '\n' ' ')
check "Option+Z: it does, then not" "on off "
got=$(BLIT_INIT="$dir/vim.lua" "$blit" --offscreen 900x500 --shot "$dir/shot.bmp" --script "frame;settingsopen;command:Toggle soft wrap;wrapdump" "$dir/wrap.c" 2>&1 >/dev/null | tail -2 | tr '\n' ' ')
check "a tab that does not wrap (the settings screen): said, and off" "blit: This tab does not wrap off "
got=$(wr "vim:3G;vim:5l;frame;statusdump;vim:gj;frame;statusdump;vim:gk;frame;statusdump;vim:j;frame;statusdump" "$dir/wrap.md" | tail -4 | sed -e 's/.*| Ln \([0-9]*\), Col \([0-9]*\).*/\1:\2/' | tr '\n' ' ')
first=$(printf '%s' "$got" | cut -d' ' -f1); second=$(printf '%s' "$got" | cut -d' ' -f2); back=$(printf '%s' "$got" | cut -d' ' -f3); next=$(printf '%s' "$got" | cut -d' ' -f4)
case "$second" in 3:*) [ "${second#3:}" -gt 10 ] && [ "$back" = "$first" ] && [ "$next" = "4:1" ] && got=rows ;; esac
check "gj and gk by rows along the row (the line the same); j by lines" "rows"
got=$(wr "vim:3G;vim:gj;frame;statusdump" "$dir/wrap.md" | tail -1 | sed -e 's/.*Col \([0-9]*\).*/\1/')
row2=$got
got=$(wr "click:100,100;frame;statusdump" "$dir/wrap.md" | tail -1 | sed -e 's/.*| Ln \([0-9]*\), Col \([0-9]*\).*/\1:\2/')
case $got in 3:*) [ "${got#3:}" -ge "$((row2 - 2))" ] && got=row ;; esac
check "a click on a wrapped row lands in its line, along that row" "row"
got=$(wr "wrapdump;vim:3G;vim:A and some more words to make it longer still<Esc>;wrapdump" "$dir/wrap.md" | tail -2 | sed -n 's/.*, \([0-9]*\) rows.*/\1/p' | tr '\n' ' ')
a=$(printf '%s' "$got" | cut -d' ' -f1); b=$(printf '%s' "$got" | cut -d' ' -f2)
[ "${b:-0}" -gt "${a:-0}" ] && got=more
check "typed on: more rows" "more"
# Six splits of one file (each a wrap that hears its edits, with the
# file's colours): the one typed in last keeps its map, as one split does.
mkdir -p "$dir/six" && cp "$dir/wrap.md" "$dir/six/"
more="vim:3G;vim:A and some more words to make it longer still<Esc>;frame;wrapdump"
one=$(wr "$more" "$dir/six/wrap.md" | tail -1 | sed -n 's/.*, \([0-9]*\) rows.*/\1/p')
split="key:cmd+shift+\\;text:wrap.md;findwait;key:Return"
got=$(BLIT_INIT="$dir/vim.lua" "$blit" --offscreen 900x1200 --shot "$dir/shot.bmp" --script "workdir:$dir/six;frame;$split;$split;$split;$split;$split;frame;$more" "$dir/six/wrap.md" 2>&1 >/dev/null | tail -1 | sed -n 's/.*, \([0-9]*\) rows.*/\1/p')
check "six splits of a file: each one's wrap hears the edits" "$one"
printf 'int f() {\n    // %s\n    return 1;\n}\nint g;\n' "$long" >"$dir/fold.c"
if wr "command:Fold every block;folddump" "$dir/fold.c" | tail -1 | grep -q '^folds none'; then
    echo "soft wrap with folds skipped: nothing folds here (no grammar for C)"
else
    got=$(wr "key:alt+z;command:Fold every block;frame;wrapdump;vim:gg;vim:j;frame;statusdump" "$dir/fold.c" | tail -2 | sed -e 's/^width [0-9]*, \([0-9]*\) rows: \(.*\)/\1 rows: \2/' -e 's/.*| Ln \([0-9]*\),.*/Ln \1/' | tr '\n' ' ')
    check "folded lines take no rows (wrapped or not); j passes over them" "4 rows: 1@0 4@0 5@0 6@0 Ln 4 "
    # Folded again with nothing new to fold (the folds listen afresh), then
    # a line opened above the fold before the next frame: the wrap measures
    # it by the folds as that edit moved them, the header still on show.
    got=$(wr "key:alt+z;command:Fold every block;frame;command:Fold every block;vim:ggO<Esc>;frame;wrapdump" "$dir/fold.c" | tail -1 | sed -e 's/^width [0-9]*, //')
    check "an edit straight after folding again: the folds have moved first" "5 rows: 1@0 2@0 5@0 6@0 7@0"
fi
printf '%s\nblit.opt.editor.modal = true\nblit.opt.editor.soft_wrap = true\n' "$base" >"$dir/wrapall.lua"
got=$(wr "wrapdump" "$dir/wrap.c" wrapall | tail -1 | sed -e 's/^width .*/on/')
check "editor.soft_wrap = true: every file" "on"

[ "$failed" = 0 ] && echo "edit: all passed"
exit "$failed"
