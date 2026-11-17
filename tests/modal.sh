#!/bin/sh
# Modal editing through the real event handlers: key presses and typed text
# as the window system delivers them, then where that leaves things. (What
# each Vim command does to a text is tested key by key in tests/fuzz_buffer.cpp;
# this is about the wiring: which keys reach Vim, and what the application
# does when Vim asks it to save, close, split.)
#
#   tests/modal.sh path/to/blit            run the cases
#   tests/modal.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/modal.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
fresh() { printf 'hello world\nsecond line\n    third\n' >"$dir/a.txt"; printf 'other\n' >"$dir/b.txt"; printf 'int x;\n    int y;\n' >"$dir/c.c"; printf 'x = 1\n' >"$dir/d.py"; printf 'a.b axb [a.b]\n' >"$dir/e.txt"; printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa b\n' >"$dir/f.txt"; }

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: the last line the script prints (vimdump or dump).
run() {
    fresh
    got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "$1" "$dir/a.txt" 2>&1 >/dev/null | tail -1)
}

run "frame;vimdump"
check "it starts in Normal mode, on the first character" 'NORMAL |  | 1:1 | hello world'
run "frame;text:x;vimdump"
check "a typed character is a command, not text" 'NORMAL |  | 1:1 | ello world'
run "frame;text:w;text:i;text:big ;vimdump"
check "i, and typing types" 'INSERT | -- INSERT -- | 1:11 | hello big world'
run "frame;text:w;text:i;text:big ;key:Escape;vimdump"
check "Esc is back to Normal mode, one character back" 'NORMAL |  | 1:10 | hello big world'
run "frame;text:A;text:!;key:Return;text:new;key:Escape;text:k;vimdump"
check "Return in Insert mode makes a line; k moves up" 'NORMAL |  | 1:3 | hello world!'
run "frame;text:i;text:ab;key:Backspace;key:Left;text:X;key:Escape;vimdump"
check "Backspace and the arrows in Insert mode" 'NORMAL |  | 1:1 | Xahello world'
run "frame;key:Down;key:Right;key:Right;text:d;text:d;vimdump"
check "the arrow keys move in Normal mode; dd keeps the column" 'NORMAL |  | 2:3 |     third'
run "frame;text:d;text:d;key:cmd+Z;vimdump"
check "Cmd+Z undoes" 'NORMAL |  | 1:1 | hello world'
run "frame;text:v;text:e;vimdump"
check "Visual mode" 'VISUAL | -- VISUAL -- | 1:5 | hello world'
run "frame;text:v;text:e;text:d;vimdump"
check "and an operator on it" 'NORMAL |  | 1:1 |  world'
run "frame;click:200,90,1;release;text:x;vimdump"
check "a click puts the cursor on a character" 'NORMAL |  | 3:8 |     thir'
run "frame;click:60,30,2;release;vimdump"
check "a double click selects a word: Visual mode" 'VISUAL | -- VISUAL -- | 1:5 | hello world'
run "frame;text:/;text:lin;vimdump"
check "a search being typed shows on the bottom line and moves the cursor" 'COMMAND | /lin | 2:8 | second line'
run "frame;text:/;text:lin;key:Return;text:n;vimdump"
check "Return keeps it; n wraps round to it again" 'NORMAL | search hit BOTTOM, continuing at TOP | 2:8 | second line'
run "frame;text:/;text:lin;key:Escape;vimdump"
check "Esc gives it up" 'NORMAL |  | 1:1 | hello world'
run "frame;text:j;text:w;key:cmd+F;vimdump"
check "Cmd+F: / with the word under the cursor on it, selected" 'COMMAND | /[line] | 2:8 | second line'
run "frame;text:j;text:w;key:cmd+F;text:th;vimdump"
check "what is typed takes its place" 'COMMAND | /th | 3:5 |     third'
run "frame;text:j;text:w;key:cmd+F;key:Backspace;vimdump"
check "Backspace clears it, and the line stays open" 'COMMAND | / | 2:8 | second line'
run "frame;text:j;text:w;key:cmd+F;key:Right;text:s;vimdump"
check "an arrow keeps it, to be typed onto" 'COMMAND | /lines | 2:8 | second line'
run "frame;text:v;text:e;key:cmd+F;key:Return;vimdump"
check "from Visual mode it is what is selected; Enter looks for it" 'NORMAL | search hit BOTTOM, continuing at TOP | 1:1 | hello world'
run "frame;open:$dir/e.txt;text:v;text:2;text:l;key:cmd+F;key:Return;vimdump"
check "as it stands: a dot in it is a dot (it is put there as \\.)" 'NORMAL |  | 1:10 | a.b axb [a.b]'
run "frame;open:$dir/e.txt;text:v;text:2;text:l;key:cmd+F;vimdump"
check "the pattern Cmd+F puts there" 'COMMAND | /[a\.b] | 1:10 | a.b axb [a.b]'
run "frame;text:/;text:s\\w\\+d;key:Return;vimdump"
check "a regular expression" 'NORMAL |  | 2:1 | second line'
run "frame;text::;text:%s/\\(\\w\\+\\) \\(\\w\\+\\)/\\2 \\1/;key:Return;text:g;text:g;vimdump"
check ":s with groups" 'NORMAL |  | 1:1 | world hello'
run "frame;text:/;text:\\(l\\)\\1;key:Return;vimdump"
check "a back-reference" 'NORMAL |  | 1:3 | hello world'
run "frame;text::;text:s/\\(hello \\)\\@<=world/there/;key:Return;vimdump"
check ":s with a look-behind" 'NORMAL |  | 1:1 | hello there'
run "frame;open:$dir/f.txt;text:/;text:\\(a*\\)*\\1b;key:Return;vimdump"
check "a pattern that takes the backtracker too long says so" 'NORMAL | E363: The pattern took too long here and was stopped (back-references and look-around are tried one way at a time) | 1:1 | aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa b'
run "frame;key:cmd+alt+F;text:o;key:Tab;text:0;key:cmd+Return;vimdump"
check "Cmd+Option+F, with modes too: replaced, in one step" 'NORMAL | Replaced 3 matches | 1:5 | hell0 w0rld'
run "frame;key:cmd+alt+F;text:o;key:Tab;text:0;key:cmd+Return;key:Escape;text:u;vimdump"
check "and u takes all of it back" 'NORMAL |  | 1:5 | hello world'
run "frame;text::;text:3;key:Return;vimdump"
check ":3 goes to line 3, in the same column" 'NORMAL |  | 3:1 |     third'
run "frame;text::;text:%s/o/0/g;key:Return;vimdump"
check ":%s" 'NORMAL | 3 substitutions on 2 lines | 2:1 | sec0nd line'
run "frame;text::;text:bogus;key:Return;vimdump"
check "an unknown command says so" 'NORMAL | E492: Not an editor command: bogus | 1:1 | hello world'
run "frame;key:ctrl+V;text:j;text:l;vimdump"
check "Ctrl-V: Visual Block mode" 'V-BLOCK | -- VISUAL BLOCK -- | 2:2 | second line'
run "frame;key:ctrl+V;text:j;text:l;text:d;text:j;vimdump"
check "and an operator on the rectangle" 'NORMAL |  | 2:1 | cond line'
run "frame;key:ctrl+V;text:j;text:I;text:# ;key:Escape;text:j;vimdump"
check "I in a block types on every line of it" 'NORMAL |  | 2:1 | # second line'
run "frame;text:G;key:ctrl+O;vimdump"
check "Ctrl-O goes back to where a long move started" 'NORMAL |  | 1:1 | hello world'
run "frame;text:G;key:ctrl+O;key:Tab;vimdump"
check "and Tab (Ctrl-I) forward again" 'NORMAL |  | 3:1 |     third'
run "frame;text:G;open:$dir/b.txt;key:cmd+1;key:ctrl+O;vimdump"
check "the jump list is kept while another file is in front" 'NORMAL |  | 1:1 | hello world'
run "frame;text:j;text:m;text:a;text:g;text:g;text:O;text:new;key:Escape;text:G;text:';text:a;vimdump"
check "a mark stays with its line when lines are added above" 'NORMAL |  | 3:1 | second line'
run "frame;text:i;key:ctrl+T;text:x;key:Escape;vimdump"
check "Ctrl-T in Insert mode indents the line" 'NORMAL |  | 1:5 |     xhello world'
run "frame;text:d;text:/;text:wor;key:Return;vimdump"
check "a search as an operator's motion" 'NORMAL |  | 1:1 | world'
run "frame;text:v;text:/;text:wor;key:Return;vimdump"
check "a search in Visual mode moves the end of the selection" 'VISUAL | -- VISUAL -- | 1:7 | hello world'
run "frame;text:\$;text:G;vimdump"
check "G keeps the column (here the end of the line), as Neovim does" 'NORMAL |  | 3:9 |     third'
run "frame;text:j;key:Backspace;vimdump"
check "Backspace goes back onto the line before" 'NORMAL |  | 1:11 | hello world'
run "frame;text:\$;text: ;vimdump"
check "and Space on to the next" 'NORMAL |  | 2:1 | second line'
run "frame;text:g;text:c;text:c;vimdump"
check "gcc in a file with no comment markers says so" "NORMAL | Option 'commentstring' is empty. | 1:1 | hello world"
run "frame;open:$dir/c.c;text:g;text:c;text:c;vimdump"
check "gcc in a C file" 'NORMAL |  | 1:1 | // int x;'
run "frame;open:$dir/c.c;text:g;text:c;text:j;text:j;vimdump"
check "gc with a motion: the markers line up" 'NORMAL |  | 2:1 | //     int y;'
run "frame;open:$dir/d.py;text:V;text:g;text:c;vimdump"
check "gc in Visual mode, in Python" 'NORMAL |  | 1:1 | # x = 1'
run "frame;open:$dir/b.txt;text:[;text:b;dump"
check "[b is the tab before" '>[a.txt* b.txt]'
run "frame;open:$dir/b.txt;open:$dir/c.c;text:[;text:B;text:];text:b;dump"
check "[B the first, ]b the next" '>[a.txt b.txt* c.c]'
run "frame;text:];text:d;vimdump"
check "]d with no diagnostics says so" 'NORMAL | No diagnostics in this file | 1:1 | hello world'

# Diagnostics, from the test language server next to the editor (it marks the words ERROR, WARN, INFO and HINT).
if [ -x "$(dirname "$blit")/blit_fakelsp" ]; then
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fake = { command = { "%s", "--utf8" }, languages = { "cpp" } }\n' "$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp" >"$dir/lsp.lua"
    printf 'int a;\nint b; // an ERROR here\nint c;\nint d; // and a WARN\n' >"$dir/e.cpp"
    diag() { got=$(BLIT_INIT="$dir/lsp.lua" "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "ready;diag;frame;$1;vimdump" "$dir/e.cpp" 2>&1 >/dev/null | tail -1); }
    diag "text:];text:d"
    check "]d goes to the next diagnostic" 'NORMAL |  | 2:14 | int b; // an ERROR here'
    diag "text:];text:d;text:];text:d"
    check "and to the one after" 'NORMAL |  | 4:17 | int d; // and a WARN'
    diag "text:];text:d;text:];text:d;text:];text:d"
    check "going round at the end of the file (the server puts one on the first line too)" 'NORMAL |  | 1:1 | int a;'
    diag "text:];text:d;text:];text:d;text:];text:d;text:];text:d"
    check "and on from there" 'NORMAL |  | 2:14 | int b; // an ERROR here'
    diag "text:[;text:d"
    check "[d goes back, round the start" 'NORMAL |  | 4:17 | int d; // and a WARN'
    diag "text:];text:D;key:ctrl+O"
    check "]D is the last, and Ctrl-O comes back from it" 'NORMAL |  | 1:1 | int a;'
fi

# What Vim asks of the application.
fresh
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;text:x;text::;text:w;key:Return;vimdump" "$dir/a.txt" 2>&1 >/dev/null | tail -1)
got="$got / $(head -1 "$dir/a.txt")"
check ":w saves" 'NORMAL |  | 1:1 | ello world / ello world'
run "frame;text::;text:e b.txt;key:Return;dump"
check ":e opens a file in a tab" '>[a.txt b.txt*]'
run "frame;text::;text:e b.txt;key:Return;text:g;text:T;dump"
check "gT goes to the tab before" '>[a.txt* b.txt]'
run "frame;text::;text:e b.txt;key:Return;text::;text:q;key:Return;dump"
check ":q closes the tab" '>[a.txt*]'
run "frame;text:x;text::;text:q!;key:Return;dump"
check ":q! closes without asking; the last tab leaves a new file" '>[untitled*]'
run "frame;key:ctrl+W;text:v;dump"
check "Ctrl-W v splits" 'H([a.txt*] | >[untitled*])0.50'
run "frame;key:ctrl+W;text:v;key:ctrl+W;text:h;dump"
check "Ctrl-W h gives the keyboard to the split on the left" 'H(>[a.txt*] | [untitled*])0.50'
run "frame;key:ctrl+W;text:s;key:ctrl+W;text:c;dump"
check "Ctrl-W s splits below, Ctrl-W c closes it again" '>[a.txt*]'
run "frame;text:i;text:typed;key:ctrl+W;text:v;key:ctrl+W;text:h;vimdump"
check "in Insert mode Ctrl-W deletes a word, and v and h are letters" 'INSERT | -- INSERT -- | 1:2 | hhello world'
run "frame;text:i;text:abc;open:$dir/b.txt;text:x;vimdump"
check "another file comes to the front: Normal mode there" 'NORMAL |  | 1:1 | ther'
run "frame;text:i;text:abc;open:$dir/b.txt;key:cmd+1;vimdump"
check "and the one left behind was left in Normal mode too" 'NORMAL |  | 1:3 | abchello world'

# With modal off it is the editor it was.
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
fresh
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;text:x;key:End;text:!;save" "$dir/a.txt" 2>&1 >/dev/null | tail -1)
got="$got$(head -1 "$dir/a.txt")"
check "modal = false: typing types" 'xhello world!'

[ -n "$print" ] || [ $failed = 1 ] || echo "modal ok"
exit $failed
