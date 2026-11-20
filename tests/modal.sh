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
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.syntax.grammars.systemverilog = { extensions = { "sv" } }\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
fresh() { printf 'hello world\nsecond line\n    third\n' >"$dir/a.txt"; printf 'other\n' >"$dir/b.txt"; printf 'int x;\n    int y;\n' >"$dir/c.c"; printf 'x = 1\n' >"$dir/d.py"; printf 'a.b axb [a.b]\n' >"$dir/e.txt"; printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa b\n' >"$dir/f.txt"; awk 'BEGIN { for (i = 1; i <= 300; i++) print "line " i }' >"$dir/n.txt"; }

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
run "frame;click:60,45,2;release;vimdump"
check "a double click selects a word: Visual mode" 'VISUAL | -- VISUAL -- | 1:5 | hello world'
run "frame;text:/;text:lin;vimdump"
check "a search being typed shows on the bottom line and moves the cursor" 'COMMAND | /lin | 2:8 | second line'
run "frame;text:/;text:lin;key:Return;text:n;vimdump"
check "Return keeps it; n wraps round to it again" 'NORMAL | search hit BOTTOM, continuing at TOP | 2:8 | second line'
run "frame;text:/;text:lin;key:Escape;vimdump"
check "Esc gives it up" 'NORMAL |  | 1:1 | hello world'
run "frame;text:j;text:w;keyof:Find in this file;vimdump"
check "Cmd+F: / with the word under the cursor on it, selected" 'COMMAND | /[line] | 2:8 | second line'
run "frame;text:j;text:w;keyof:Find in this file;text:th;vimdump"
check "what is typed takes its place" 'COMMAND | /th | 3:5 |     third'
run "frame;text:j;text:w;keyof:Find in this file;key:Backspace;vimdump"
check "Backspace clears it, and the line stays open" 'COMMAND | / | 2:8 | second line'
run "frame;text:j;text:w;keyof:Find in this file;key:Right;text:s;vimdump"
check "an arrow keeps it, to be typed onto" 'COMMAND | /lines | 2:8 | second line'
run "frame;text:v;text:e;keyof:Find in this file;key:Return;vimdump"
check "from Visual mode it is what is selected; Enter looks for it" 'NORMAL | search hit BOTTOM, continuing at TOP | 1:1 | hello world'
run "frame;open:$dir/e.txt;text:v;text:2;text:l;keyof:Find in this file;key:Return;vimdump"
check "as it stands: a dot in it is a dot (it is put there as \\.)" 'NORMAL |  | 1:10 | a.b axb [a.b]'
run "frame;open:$dir/e.txt;text:v;text:2;text:l;keyof:Find in this file;vimdump"
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
run "frame;open:$dir/d.py;text:o;key:Tab;text:y;key:Tab;text:z;key:Escape;vimdump"
check "Tab in Insert mode puts in the file's indentation: spaces to the next level" 'NORMAL |  | 2:9 |     y   z'
run "frame;text:l;text:R;key:Tab;text:X;key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Escape;vimdump"
check "and in Replace mode Backspace takes it back out" 'NORMAL |  | 1:1 | hello world'
# typed FILE SCRIPT: the file after the script and a save, its lines joined by "|" (a tab shown as ">").
typed() {
    fresh
    printf 'module m;\nendmodule\n' >"$dir/m.sv"
    printf 'if x then\nend\n' >"$dir/l.lua"
    "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;$2;save" "$dir/$1" >/dev/null 2>&1
    got=$(tr '\t\n' '>|' <"$dir/$1")
}
typed d.py "text:o;text:def f(rows):;key:Return;text:for r in rows:  # each;key:Return;text:if r:;key:Return;text:return 1;key:Return;text:else:;key:Return;text:pass;key:Return;text:y = [;key:Return;text:1,;key:Return;text:];key:Escape"
check "Python: Enter after a colon goes a level in, after return a level out; else and ] go out" 'x = 1|def f(rows):|    for r in rows:  # each|        if r:|            return 1|        else:|            pass|        y = [|            1,|        ]|'
typed c.c "text:o;text:if (x) {;key:Return;text:y();key:Return;text:};key:Escape;text:k;text:o;text:z;key:Escape"
check "C: after { a level in, } a level out; o goes in as Enter does" 'int x;|if (x) {|    y()|    z|}|    int y;|'
typed l.lua "text:o;text:for i = 1, 3 do;key:Return;text:f(i);key:Return;text:end;key:Return;text:g = function();key:Return;text:return 1;key:Return;text:end;key:Escape"
check "Lua: then, do and function open; end closes (Enter after it, or Esc)" 'if x then|>for i = 1, 3 do|>>f(i)|>end|>g = function()|>>return 1|>end|end|'
typed m.sv "text:o;text:always_comb begin;key:Return;text:unique case (s);key:Return;text:0: y = 1;key:Return;text:endcase ;key:Return;text:end else begin;key:Return;text:y = 0;key:Return;text:end;key:Escape"
check "SystemVerilog: begin and case open, endcase and end close" 'module m;|>always_comb begin|>>unique case (s)|>>>0: y = 1|>>endcase |>end else begin|>>y = 0|>end|endmodule|'
run "frame;text:d;text:/;text:wor;key:Return;vimdump"
check "a search as an operator's motion" 'NORMAL |  | 1:1 | world'
run "frame;text:v;text:/;text:wor;key:Return;vimdump"
check "a search in Visual mode moves the end of the selection" 'VISUAL | -- VISUAL -- | 1:7 | hello world'
run "frame;text:\$;text:G;vimdump"
check "G keeps the column (here the end of the line), as Neovim does" 'NORMAL |  | 3:9 |     third'
run "frame;open:$dir/n.txt;text:/;text:line 150;key:Return;rowsdump:1"
check "a search's match out of view is shown a third of the way down" 'rows | line 145'
run "frame;open:$dir/n.txt;text:G;text:?;text:line 200;key:Return;rowsdump:1"
check "above the view too" 'rows | line 195'
run "frame;open:$dir/n.txt;text:3;text:0;text:j;rowsdump:1"
check "a move that is no jump takes the view only as far as it must" 'rows | line 14'
run "frame;open:$dir/n.txt;lua:blit.opt.editor.scrolloff = 3;text:3;text:0;text:j;rowsdump:1"
check "editor.scrolloff keeps lines in view past the cursor" 'rows | line 17'
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
fresh
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;text:x;text::;text:w|noh;key:Return;vimdump;tabsdump" "$dir/a.txt" 2>&1 >/dev/null | tail -2 | tr '\n' ' ')
got="$got/ $(head -1 "$dir/a.txt") $(ls "$dir" | grep -c noh)"
check ":w|noh writes nothing: | is refused, not taken into the name" 'NORMAL | E488: Trailing characters: |noh | 1:1 | ello world a.txt / hello world 0'
run "frame;text:x;text::;text:noh|w;key:Return;vimdump"
check "nor is what follows it left out without a word" 'NORMAL | E488: Trailing characters: |w | 1:1 | ello world'
run "frame;text::;text:s/l/L/g|s/o/0/g;key:Return;vimdump"
check ":s with | after its flags runs neither" 'NORMAL | E488: Trailing characters: |s/o/0/g | 1:1 | hello world'
run "frame;text::;text:s/x\\|w/0/;key:Return;vimdump"
check "while | inside the pattern is the pattern's" 'NORMAL |  | 1:1 | hello 0orld'
fresh
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;text:x;text:j;text::;text:e!;key:Return;text:k;vimdump;tabsdump;statusdump" "$dir/a.txt" 2>&1 >/dev/null | tail -3 | cut -c1-40 | tr '\n' ' ' | sed 's/ *$//')
check ":e! reads the file again, its changes dropped (no new tab)" 'NORMAL |  | 1:1 | hello world a.txt NORMAL a.txt | Ln 1, Col 1 Spaces: 4 LF'
run "frame;text::;text:e b.txt;key:Return;dump"
check ":e opens a file in a tab" '>[a.txt b.txt*]'
# gf SCRIPT [FILE]: beside the file first, then the working folder; a
# name in neither is said so. (Run from the file's folder, FILE as named.)
gf() {
    fresh
    mkdir -p "$dir/gf/sh/include" "$dir/gf/include"
    printf 'float x;\nfloat y;\n' >"$dir/gf/sh/include/common.glsl"
    printf '#version 450\n#include "include/common.glsl"\n#include "missing.h"\nsh/include/common.glsl:2: here\ninclude\n' >"$dir/gf/sh/a.frag"
    got=$(cd "$dir/gf/sh" && "$exe" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "workdir:$dir/gf;frame;$1;vimdump;dump" "${2:-$dir/gf/sh/a.frag}" 2>&1 >/dev/null | tail -2 | tr '\n' ' ')
}
exe=$(cd "$(dirname "$blit")" && pwd)/$(basename "$blit")  # (gf runs it from the file's folder)
gf "text:j;text:\$;text:h;text:h;text:gf"
check "gf on a quoted include: the file beside the one it is in" 'NORMAL |  | 1:1 | float x; >[a.frag common.glsl*] '
gf "text:j;text:j;text:\$;text:h;text:h;text:gf"
check "a name found nowhere is said so, and no tab is opened" 'NORMAL | No file missing.h | 3:18 | #include "missing.h" >[a.frag*] '
gf "text:G;text:gf"
check "nor for a folder (include is one, in the working folder)" 'NORMAL | No file include | 5:1 | include >[a.frag*] '
gf "text:4;text:G;text:gf"
check "path:line, from the working folder, at that line" 'NORMAL |  | 2:1 | float y; >[a.frag common.glsl*] '
gf "text::;text:e /nonexistent/dir/x.c;key:Return"
check ":e in a folder that is not there says so" 'NORMAL | No file /nonexistent/dir/x.c | 1:1 | #version 450 >[a.frag*] '
gf "text:j;text:\$;text:h;text:h;text:gf" a.frag
check "and beside it when it was named from its own folder (blit a.frag)" 'NORMAL |  | 1:1 | float x; >[a.frag common.glsl*] '
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
fresh
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;key:Tab;text:x;key:Down;key:Down;key:Home;key:Tab;save" "$dir/a.txt" 2>&1 >/dev/null | tail -1)
got="$got$(head -3 "$dir/a.txt" | tr '\n' '|')"
check "modal = false: Tab puts in the file's indentation too" '    xhello world|second line|        third|'

[ -n "$print" ] || [ $failed = 1 ] || echo "modal ok"
exit $failed
