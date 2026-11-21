#!/bin/sh
# Sessions: a project folder opened again brings back what was open in it
# (the tabs and their order, the splits and their sizes, each file's cursor
# and view, the side panel's tab and width, the Build tab's task), through
# the real key and text events, the start screen, `blit DIR` and the box
# that opens folders. Kept in a scratch folder beside the file
# BLIT_RECENT_FILE names, never the real one; files on another machine are
# read through tests/fake-ssh.sh, as tests/remote.sh does.
#
#   tests/session.sh path/to/blit            run the cases
#   tests/session.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/session.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
V="$dir/other"
T="$dir/there"  # another machine's home (tests/fake-ssh.sh)
mkdir -p "$W/src" "$V" "$T/far" "$dir/home/.ssh"
seq 1 300 | sed 's/^/line /' >"$W/long.txt"
printf 'notes\nsecond line\nthird\n' >"$W/notes.txt"
printf 'int main() {}\n' >"$W/src/main.cpp"
printf 'all:\n\techo all\ntest:\n\techo test\n' >"$W/Makefile"
printf 'other\n' >"$V/x.txt"
printf 'one\ntwo\nthree\n' >"$T/far/a.txt"
printf 'Host box\n    HostName example.invalid\n' >"$dir/home/.ssh/config"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' >"$dir/off.lua"
export BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"  # (a shell that is the same everywhere, as tests/term.sh has)
S="$dir/sessions"
R="ssh://box$T/far"

failed=0
check() {
    # (The status bar's icons beside a branch and another machine are the
    # font's, where it has them: the words are what is compared.)
    got=$(printf '%s' "$got" | sed -e 's/⎇ //g' -e 's/⇄ //g' -e 's/ //g')
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
join() { awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }'; }
# run SCRIPT [FILE]: what the run printed, its lines joined by " ;; ". INIT,
# if set, is the init.lua for this run only. (Not "VAR=x run": sh keeps
# such an assignment after a function.)
# The end of the file: Cmd+Down on a Mac, Ctrl+End elsewhere.
case $(uname) in Darwin) doc_end=cmd+Down ;; *) doc_end=ctrl+End ;; esac
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" | join)
    INIT=
}
# kept FOLDER: its session as written, its lines joined.
kept() {
    f=$(grep -lx "folder	$1" "$S"/* 2>/dev/null | head -1)
    got=$([ -n "$f" ] && sed -e "s#$dir#~#g" "$f" | join)
}

# What is kept, and when.
run "open:$W/notes.txt;quit"
got=$(ls "$S" 2>/dev/null | wc -l | tr -d ' ')
check "a file opened, no folder opened to work in: nothing kept" '0'
run "folder:$W;open:$W/notes.txt;key:Down;key:End;open:$W/long.txt;key:$doc_end;key:cmd+\\;open:$W/src/main.cpp;key:Right;key:Right;frame"
got=$(ls "$S" 2>/dev/null | wc -l | tr -d ' ')
check "not before it quits" '0'
run "folder:$W;open:$W/notes.txt;key:Down;key:End;open:$W/long.txt;key:$doc_end;key:cmd+\\;open:$W/src/main.cpp;key:Right;key:Right;frame;buildrun:make test;buildwait;quit"
kept "$W"
check "on quit: the splits, the tabs in order, the cursors and views, the panel, the Build tab's task" \
    'blit session 1 ;; folder	~/work ;; tabs	3 ;; panel	0	Files	32 ;; build	Make	make test ;; H	0.5000 ;; pane	1	0 ;; file	1	11	0.00	0.00	~/work/notes.txt ;; file	300	0	281.10	0.00	~/work/long.txt ;; pane	0	1 ;; file	0	2	0.00	0.00	~/work/src/main.cpp'

# Brought back.
run "frame;dump;statusdump;builddump" "$W"
check "blit DIR: as it was left" \
    'H([notes.txt long.txt*]@282 | >[main.cpp*])0.50 ;; src/main.cpp | Ln 1, Col 3 Tabs LF C++ ;; Make: make make all >make test'
run "frame;key:cmd+alt+Left;key:cmd+1;frame;statusdump" "$W"
check "each file's cursor" 'notes.txt | Ln 2, Col 12 Tabs LF Plain text'
run "frame;startdump"
check "Recent says what will come back" \
    'start >n:New file p:New project… o:Open a file… f:Go to file s:Search in files d:Open a folder… r:Connect to a server… c:Settings q:Quit 1:~/work/(3) 2:~/work/src/main.cpp 3:~/work/long.txt 4:~/work/notes.txt'
run "frame;text:1;frame;dump;startdump"
check "and the start screen's Recent brings it back" 'H([notes.txt long.txt*]@282 | >[main.cpp*])0.50 ;; no start screen'
run "workdir:$dir;frame;text:d;text:work;key:Return;key:Return;frame;dump"
check "so does the box that opens folders (d)" 'H([notes.txt long.txt*]@282 | >[main.cpp*])0.50'
INIT="$dir/off.lua"
run "frame;dump" "$W"
check "editor.restore = false: nothing comes back" '>[untitled*]'
INIT="$dir/off.lua"
run "frame;startdump"
check "and Recent says nothing of it" \
    'start >n:New file p:New project… o:Open a file… f:Go to file s:Search in files d:Open a folder… r:Connect to a server… c:Settings q:Quit 1:~/work/ 2:~/work/src/main.cpp 3:~/work/long.txt 4:~/work/notes.txt'

# The panel's tab and width, as kept.
f=$(grep -lx "folder	$W" "$S"/*)
sed -e 's/^panel	.*/panel	1	Build	40/' "$f" >"$f.new" && mv "$f.new" "$f"
run "frame;sessiondump" "$W"
got=$(printf '%s' "$got" | sed -e 's/ ;; build.*//')
check "the side panel: shown, its tab, its width" 'blit session 1 ;; folder	~/work ;; tabs	3 ;; panel	1	Build	40'

# What is gone stays gone.
mv "$W/notes.txt" "$dir/notes.txt"
run "frame;dump" "$W"
check "a file that is gone is left out" 'H([long.txt*]@282 | >[main.cpp*])0.50'
mv "$W/src/main.cpp" "$dir/main.cpp"
run "frame;dump" "$W"
check "a split left with nothing goes" '>[long.txt*]@282'
mv "$dir/notes.txt" "$W/notes.txt"
mv "$dir/main.cpp" "$W/src/main.cpp"

# Another folder opened: the last one's session is kept then, and the window is this one's.
run "folder:$V;open:$V/x.txt;quit"
run "folder:$W;frame;key:cmd+alt+Left;key:Up;folder:$V;frame;dump"
check "another folder: its tabs in place of the last one's" '>[x.txt*]'
kept "$W"
got=$(printf '%s' "$got" | sed -e 's/.*\(file	[^;]*long.txt\).*/\1/')
check "and the last one's kept as it was then" 'file	299	0	281.32	0.00	~/work/long.txt'
run "folder:$W;frame;key:cmd+alt+Left;text:zz;folder:$V;frame;dump"
check "a file with changes not saved stays (in the split with the keyboard)" '>[x.txt* long.txt]'
run "folder:$V;key:ctrl+\`;frame;termwait:\$;key:space;text:echo kep''t;key:Return;termwait:kept;folder:$W;frame;dump;key:ctrl+\`;frame;termwait:kept;dump"
check "the terminals' split along the bottom stays as it was, its shell still running" \
    'H(V(>[notes.txt long.txt*]@282 / [Terminal*])0.70 | [main.cpp*])0.50 ;; H(V([notes.txt long.txt*]@282 / >[Terminal*])0.70 | [main.cpp*])0.50'

# A view's top is kept as a line and the part through it, not as rows: a
# file that wraps comes back wider with the same line at the top, and one
# with blocks folded above its top comes back (its folds open) with that
# line at the top. A tab not in front keeps its line too.
X="$dir/wraps"
mkdir -p "$X"
i=1; while [ $i -le 100 ]; do echo "Line $i of the file, which goes on with words for long enough that it wraps onto a second row and a third one in a view that is narrow"; i=$((i + 1)); done >"$X/wrapped.md"
i=1; while [ $i -le 60 ]; do printf 'int f%d() {\n    int a = 1;\n    int b = 2;\n    return a + b;\n}\n' $i; i=$((i + 1)); done >"$X/blocks.c"
top() { sed 's/.* rows: \([0-9]*\)@.*/\1/'; }
run "folder:$X;open:$X/wrapped.md;line:62;frame;wrapdump;quit"
got=$(printf '%s' "$got" | top)
check "a file that wraps: line 21 at the top, its second row" '21'
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "frame;wrapdump" "$X" 2>&1 >/dev/null | tail -1 | top)
check "brought back wider (fewer rows to a line): line 21 at the top" '21'
run "folder:$X;open:$X/blocks.c;command:Fold every block;line:21;frame;quit"
kept "$X"
got=$(printf '%s' "$got" | sed 's/.*;; pane[^;]*;; //')
check "kept: each tab's top line (the one not in front's too)" 'file	0	0	20.33	0.00	~/wraps/wrapped.md ;; file	0	0	50.00	0.00	~/wraps/blocks.c'
run "frame;dump" "$X"
check "folded above the top, brought back open: line 51 at the top" '>[wrapped.md blocks.c*]@51'
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "frame;key:cmd+1;frame;wrapdump" "$X" 2>&1 >/dev/null | tail -1 | top)
check "and the tab behind it, wider: line 21 at the top" '21'

# One file in two splits: both views come back, each where it was.
rm -f "$S"/*
run "folder:$W;open:$W/long.txt;key:Down;key:Down;key:cmd+\\;text:long.txt;findwait;key:Return;key:Down;key:Down;key:Down;quit"
run "folder:$W;frame;dump;views"
check "a file two splits showed: two views of it again, each with its own cursor" \
    'H([long.txt*] | >[long.txt*])0.50 ;; views: [long.txt#3 2:0 line 3] [long.txt#3 5:0 line 6]<'

# A notebook: its view beside it again, the keyboard on its script.
rm -f "$S"/*
printf '{"cells": [{"cell_type": "code", "execution_count": 1, "metadata": {}, "outputs": [], "source": ["x = 1"]}], "metadata": {}, "nbformat": 4, "nbformat_minor": 5}\n' >"$W/n.ipynb"
run "folder:$W;open:$W/n.ipynb;mdwait;quit"
run "folder:$W;frame;mdwait;dump"
check "a notebook: its view beside it again" 'H(>[n.ipynb*] | [Notebook: n.ipynb*])0.50'
rm "$W/n.ipynb"

# Another machine's folder: its files come back when the connection is up.
run "folder:$R;open:$R/a.txt;remotewait;key:Down;key:Right;quit"
run "remotewait;frame;dump;statusdump" "$R"
check "another machine's: back when it is here, the cursor where it was" '>[a.txt*] ;; box a.txt | Ln 2, Col 2 Tabs LF Plain text'
rm "$T/far/a.txt"
run "remotewait;frame;dump" "$R"
check "and one gone there goes quietly" '>[untitled*]'

# The breakpoints (with what each waits for: a condition with a backslash
# in it, a message), the functions' and the expressions watched: kept, and
# back; and another folder's in their place.
C='breakif:3;text:s != "\t";key:Return;key:Return;text:x is {x};key:Return'
run "folder:$W;open:$W/notes.txt;break:2;$C;command:Break at a function…;key:ctrl+U;text:square;key:Return;debugpanel;debugpick:add;text:x * 100;key:Return;quit"
f=$(grep -lx "folder	$W" "$S"/* 2>/dev/null | head -1)
got=$(grep -E '^(break|function|watch)	' "$f" | sed -e "s#$dir#~#g" | join)
check "kept: the breakpoints, the functions' and the watches" \
    'break	1				~/work/notes.txt ;; break	2	s != "\\t"		x is {x}	~/work/notes.txt ;; function	square ;; watch	x * 100'
run "debugbreaks;debugdump" "$W"
check "back: as they were" 'notes.txt:2; notes.txt:3 if s != "\t" log x is {x}; square() ;; idle; watch: x * 100'
run "folder:$V;open:$V/x.txt;break:1;quit"
run "folder:$W;debugbreaks;folder:$V;debugbreaks;debugdump"
check "another folder's in their place" 'notes.txt:2; notes.txt:3 if s != "\t" log x is {x}; square() ;; x.txt:1 ;; idle'

# The language chosen for a file from the status bar: kept, and back, in
# each split that shows it; a file whose language is its name's keeps none.
printf 'SECTIONS { }\n' >"$W/link.txt"
run "folder:$W;open:$W/link.txt;frame;statusclick:Plain;text:python;key:Return;key:cmd+\\;open:$W/notes.txt;quit"
f=$(grep -lx "folder	$W" "$S"/* 2>/dev/null | head -1)
got=$(grep '^lang	' "$f" | sed -e "s#$dir#~#g" | join)
check "kept: the language chosen, once for the file" 'lang	python	~/work/link.txt'
run "frame;key:cmd+alt+Left;frame;statusdump;lua:print(blit.info(0).language)" "$W"
got=$(printf '%s' "$got" | sed -e 's/^.* LF //' -e 's/ (no colours)//' -e 's/blit: //')  # (whether its grammar is installed is this machine's)
check "back: the file is that language again" 'Python ;; python'
run "folder:$W;open:$W/link.txt;frame;statusclick:Python;text:plain;key:Return;quit"
f=$(grep -lx "folder	$W" "$S"/* 2>/dev/null | head -1)
got=$(grep '^lang	' "$f" | sed -e "s#$dir#~#g" | join)
check "plain text chosen is kept too" 'lang	-	~/work/link.txt'

[ -n "$print" ] || [ $failed = 1 ] || echo "session ok"
exit $failed
