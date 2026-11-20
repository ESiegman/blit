#!/bin/sh
# Finding files, text and paths, through the real key and text events: what
# the box lists, what choosing something does, and that the system's file
# dialogs are no longer what opens, saves and chooses a folder.
#
#   tests/find.sh path/to/blit            run the cases
#   tests/find.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/find.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
W="$dir/work"
fresh() {
    rm -rf "$W"
    mkdir -p "$W/src/deep" "$W/docs" "$W/build" "$W/node_modules/x"
    printf 'alpha line\nthe needle is here\nomega\n' >"$W/src/main.cpp"
    printf 'int helper();\n// another Needle, with a capital\n' >"$W/src/helper.hpp"
    printf 'deep down\n    indented needle line\n' >"$W/src/deep/inner.txt"
    printf 'notes about things\n' >"$W/docs/notes.md"
    printf 'needle in a built file\n' >"$W/build/out.txt"
    printf 'needle in a module\n' >"$W/node_modules/x/index.js"
    printf 'needle in a log\n' >"$W/run.log"
    printf '*.log\n' >"$W/.gitignore"
    printf 'top\n' >"$W/README.md"
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
# run SCRIPT: the last line the script prints (finddump, dump or vimdump).
run() {
    fresh
    got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;$1" "$W/README.md" 2>&1 >/dev/null | tail -1)
}

# Files by name.
run "key:cmd+P;finddump"
check "Cmd+P with nothing typed: the files open, then those opened lately" 'names [] 1 of 6 : Open: >README.md'
run "key:cmd+P;text:.;finddump"
check "Cmd+P finds the files, without what .gitignore and the usual rules leave out" 'names [.] 6 of 6 : >.gitignore README.md docs/notes.md src/helper.hpp src/main.cpp src/deep/inner.txt'
run "key:cmd+P;text:main;finddump"
check "what is typed narrows it" 'names [main] 1 of 1 : >src/main.cpp'
run "key:cmd+P;text:hlp;finddump"
check "the letters need not be next to one another" 'names [hlp] 1 of 1 : >src/helper.hpp'
run "key:cmd+P;text:src e;finddump"
check "every word typed has to match; the best match is first" 'names [src e] 2 of 2 : >src/helper.hpp src/deep/inner.txt'
run "key:cmd+P;text:inner;key:Return;dump"
check "Enter opens the one chosen, in a tab" '>[README.md inner.txt*]'
# The row chosen: its folder in an ink of its own, one that reads on that
# row's background (4.5:1), where the muted ink of the others would not.
export BLIT_APPEARANCE=dark
run "key:cmd+P;text:src e;findinks"
check "the chosen row's folder in an ink that reads there, dark" 'muted #7f848e 3.92, chosen #c4c8c8 4.85'
BLIT_APPEARANCE=light
run "key:cmd+P;text:src e;findinks"
check "and light" 'muted #75777e 4.03, chosen #535459 4.95'
# (The muted ink of the two light themes is a step darker than it was, 4:1
# on a box's background: blit-light's, and rose-pine-dawn's, its palette's
# subtle.)
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.theme.light = "rose-pine-dawn"\n' >"$dir/dawn.lua"
BLIT_INIT="$dir/dawn.lua"
run "key:cmd+P;text:src e;findinks"
check "and rose-pine-dawn" 'muted #797593 4.23, chosen #605b80 4.61'
BLIT_INIT="$dir/init.lua"
unset BLIT_APPEARANCE
run "key:cmd+P;text:s;key:Down;key:Return;dump"
check "Down chooses the next" '>[README.md helper.hpp*]'
run "key:cmd+P;text:zzzz;finddump"
check "nothing found" 'names [zzzz] 0 of 0 :'
run "key:cmd+P;text:main;key:Escape;finddump"
check "Esc shuts it" 'closed'
run "key:cmd+P;key:cmd+P;finddump"
check "and so does the key that opened it" 'closed'
run "key:cmd+P;text:top;key:Escape;vimdump"
check "what was typed went into the box, not the file" 'NORMAL |  | 1:1 | top'
run "key:cmd+P;text:mainx;key:Backspace;key:Return;dump"
check "Backspace takes a letter back" '>[README.md main.cpp*]'
run "key:cmd+P;text:inner;click:5,5;release;finddump"
check "a click beside the box shuts it" 'closed'

# Text in files.
run "key:cmd+shift+F;text:needle;finddump"
check "Cmd+Shift+F: the lines a word is on, either case, in the order of the files" 'text [needle] 3 of 3 : >src/deep/inner.txt:2:14 src/helper.hpp:2:12 src/main.cpp:2:5'
run "key:cmd+shift+F;text:Needle;finddump"
check "a capital typed is a capital found" 'text [Needle] 1 of 1 : >src/helper.hpp:2:12'
run "key:cmd+shift+F;text:here needle;finddump"
check "words in any order, all on the line" 'text [here needle] 1 of 1 : >src/main.cpp:2:5'
run "key:cmd+shift+F;text:nee;findwait;text:d;findwait;text:le;findwait;text:sx;findwait;key:Backspace;findwait;key:Backspace;finddump"
check "Backspace after searches typed onto each other finds what the shorter word finds" 'text [needle] 3 of 3 : >src/deep/inner.txt:2:14 src/helper.hpp:2:12 src/main.cpp:2:5'
run "key:cmd+shift+F;text:here needle;findwait;key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace;finddump"
check "and back past where the words were typed: found again" 'text [here] 1 of 1 : >src/main.cpp:2:15'
run "key:cmd+shift+F;text:n;finddump"
check "one letter is not searched for" 'text [n] 0 of 0 :'
run "key:cmd+shift+F;text:needle;key:Down;key:Down;key:Return;vimdump"
check "Enter goes to the line, with the cursor on the word" 'NORMAL |  | 2:5 | the needle is here'
run "key:cmd+shift+F;text:indented;key:Return;key:ctrl+O;dump"
check "and Ctrl-O comes back from it" '>[README.md* inner.txt]'
run "key:cmd+P;text:needle;key:Tab;finddump"
check "Tab turns a search for names into one for text, with what was typed" 'text [needle] 3 of 3 : >src/deep/inner.txt:2:14 src/helper.hpp:2:12 src/main.cpp:2:5'
run "text::;text:find helper;key:Return;finddump"
check ":find" 'names [helper] 1 of 1 : >src/helper.hpp'
run "text::;text:grep omega;key:Return;finddump"
check ":grep" 'text [omega] 1 of 1 : >src/main.cpp:3:1'

# Replacing in files: Tab (or Cmd+Shift+H) gives Search in files a second
# field; the lines then show what they would become, and what is typed is
# looked for as it stands (or as a regular expression, Option+R).
run "key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;finddump"
check "Tab: Replace with, and each line before and after" 'text [needle] with> [pin] 3 of 3 : >src/deep/inner.txt:2:14=    indented pin line src/helper.hpp:2:12=// another pin, with a capital src/main.cpp:2:5=the pin is here'
run "key:cmd+shift+H;text:Needle;findwait;finddump"
check "Cmd+Shift+H opens it with that field; a capital typed is a capital found" 'text [Needle] with [] 1 of 1 : >src/helper.hpp:2:12=// another , with a capital'
run "key:cmd+shift+F;text:needle is;findwait;key:cmd+shift+H;findwait;finddump"
check "with the field there what is typed is a phrase, as it stands" 'text [needle is] with> [] 1 of 1 : >src/main.cpp:2:5=the  here'
run "key:cmd+shift+F;key:alt+R;text:ne\\+dle\\|omega;findwait;finddump"
check "Option+R: a regular expression" 'text [ne\+dle\|omega] .* 4 of 4 : >src/deep/inner.txt:2:14 src/helper.hpp:2:12 src/main.cpp:2:5 src/main.cpp:3:1'
run "key:cmd+shift+F;key:alt+C;text:needle;findwait;finddump"
check "Option+C: case as typed" 'text [needle] Aa 2 of 2 : >src/deep/inner.txt:2:14 src/main.cpp:2:5'
run "key:cmd+shift+F;key:alt+R;text:\\(;findwait;finddump"
check "what is not a pattern says why" 'text [\(] .* (E54: Unmatched \() 0 of 0 :'
run "key:cmd+shift+F;key:alt+R;text:n\\(e\\)\\1d;findwait;finddump"
check "a back-reference" 'text [n\(e\)\1d] .* 3 of 3 : >src/deep/inner.txt:2:14 src/helper.hpp:2:12 src/main.cpp:2:5'
run "key:cmd+shift+F;key:alt+R;text:\\(the \\)\\@<=needle;findwait;finddump"
check "look-behind" 'text [\(the \)\@<=needle] .* 1 of 1 : >src/main.cpp:2:5'
run "key:cmd+shift+F;key:alt+R;text:\\(\\w\\+\\) \\(is\\);key:Tab;text:\\2 \\u\\1;findwait;finddump"
check "groups in what it becomes" 'text [\(\w\+\) \(is\)] with> [\2 \u\1] .* 1 of 1 : >src/main.cpp:2:5=the is Needle here'
run "open:$W/src/main.cpp;key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;key:cmd+Return;finddump"
check "Cmd+Enter asks first" 'text [needle] with> [pin] asking 3 in 3 3 of 3 : >src/deep/inner.txt:2:14=    indented pin line src/helper.hpp:2:12=// another pin, with a capital src/main.cpp:2:5=the pin is here'
run "open:$W/src/main.cpp;key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;key:cmd+Return;key:Escape;finddump"
check "Esc: not now" 'text [needle] with> [pin] 3 of 3 : >src/deep/inner.txt:2:14=    indented pin line src/helper.hpp:2:12=// another pin, with a capital src/main.cpp:2:5=the pin is here'
run "open:$W/src/main.cpp;key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;key:cmd+Return;key:Return;vim:j;vimdump;dump"
got="$got | $(sed -n 2p "$W/src/main.cpp") | $(sed -n 2p "$W/src/helper.hpp") | $(sed -n 2p "$W/src/deep/inner.txt")"
check "Enter: a file open in a tab is changed there and saved, the others where they are" '>[README.md main.cpp*] | the pin is here | // another pin, with a capital |     indented pin line'
run "open:$W/src/main.cpp;vim:Ax<Esc>;key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;key:cmd+Return;key:Return;vimdump"
got="$got | $(sed -n 2p "$W/src/main.cpp")"
check "one with changes of its own: changed in its tab, not saved, and said" 'NORMAL | Replaced 3 hits in 3 files; main.cpp changed, not saved (it has changes of its own) | 1:11 | alpha linex | the needle is here'
run "open:$W/src/main.cpp;key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;key:cmd+Return;key:Return;vimdump"
check "and the bottom line says how many" 'NORMAL | Replaced 3 hits in 3 files | 1:1 | alpha line'
run "open:$W/src/main.cpp;key:cmd+shift+F;text:needle;key:Tab;text:pin;findwait;key:cmd+Return;key:Return;text:j;text:u;text:j;vimdump"
check "u takes it back in the file that was open (the cursor back where it was, then down onto it)" 'NORMAL |  | 2:1 | the needle is here'

# Finding and replacing in the file in front: Cmd+Option+F (with Vim's modes as without).
run "open:$W/src/main.cpp;text:j;text:w;key:cmd+alt+F;finddump"
check "Cmd+Option+F: the word at the cursor, chosen, and Replace with" 'file [needle] with [] 1 of 1 at 15-21'
run "open:$W/src/main.cpp;key:cmd+alt+F;text:e;key:Tab;text:E;key:Return;key:Return;finddump"
check "Enter replaces the match chosen and chooses the next (small letters: either case)" 'file [e] with> [E] 3 of 8 at 16-17'
run "open:$W/src/main.cpp;key:cmd+alt+F;key:alt+R;text:\\<\\(\\w\\)\\(\\w*\\);key:Tab;text:\\u\\1\\2;key:cmd+Return;vimdump;key:Escape;key:cmd+S"
got="$got | $(sed -n 2p "$W/src/main.cpp")"
check "Cmd+Enter replaces them all, with a regular expression's groups" 'NORMAL | Replaced 7 matches | 1:1 | Alpha Line | The Needle Is Here'

# On another machine (tests/fake-ssh.sh stands in for ssh): replaced there.
T="$dir/there"
mkdir -p "$T/work/src"
printf 'the needle\n' >"$T/work/src/a.txt"
printf 'no\nneedle two needle\n' >"$T/work/b.txt"
got=$(BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$T" BLIT_RECENT_FILE="$dir/recent" "$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" \
    --script "remotewait;find:text;key:alt+R;text:ne\\+dle;key:Tab;text:pin;findwait;key:cmd+Return;key:Return;remotewait;vimdump" "ssh://box$T/work" 2>&1 >/dev/null | tail -1)
got="$got | $(cat "$T/work/src/a.txt") | $(sed -n 2p "$T/work/b.txt")"
check "a folder on another machine: its files read and written there" 'NORMAL | Replaced 3 hits in 2 files | 1:1 |  | the pin | pin two pin'

# A path, where the system's dialogs used to be.
run "keyof:Open a file;finddump"
check "Cmd+O: the working directory as it is on disk, folders first" "open [$W/] 7 : >../ build/ docs/ node_modules/ src/ README.md"
run "keyof:Open a file;text:sr;key:Return;finddump"
check "Enter on a folder goes into it" "open [$W/src/] 4 : >../ deep/ helper.hpp main.cpp"
run "keyof:Open a file;text:src/he;key:Return;dump"
check "Enter on a file opens it" '>[README.md helper.hpp*]'
run "keyof:Open a file;text:do;key:Tab;text:no;key:Tab;finddump"
check "Tab completes the name chosen" "open [$W/docs/notes.md] 1 : >notes.md"
run "keyof:Open a file;text:src/;key:Return;finddump"
check "the first row of a folder is the one above it" "open [$W/] 7 : >../ build/ docs/ node_modules/ src/ README.md"
run "keyof:Open a file;text:brand_new.txt;finddump;key:Return;dump"
check "a name that is not there opens a new file of that name" '>[README.md brand_new.txt*]'
run "keyof:Open a file;text:.g;finddump"
check "names with a dot in front show once a dot is typed" "open [$W/.g] 2 : >.gitignore run.log"
run "keyof:Open a file;text:src/deep/;key:alt+Backspace;finddump"
check "Option+Backspace goes a folder back" "open [$W/src/] 4 : >../ deep/ helper.hpp main.cpp"

# Saving a new file asks for its name here.
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;keyof:New file;text:ihello there;key:Escape;key:cmd+S;finddump" "$W/README.md" 2>&1 >/dev/null | tail -1)
check "Cmd+S in a new file asks for a name, starting in the working directory" "save [$W/] 7 : >../ build/ docs/ node_modules/ src/ README.md"
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;keyof:New file;text:ihello there;key:Escape;key:cmd+S;text:docs/new.txt;key:Return;dump" "$W/README.md" 2>&1 >/dev/null | tail -1)
got="$got / $(cat "$W/docs/new.txt" 2>/dev/null)"
check "the name typed is the file's from then on, and it is saved" '>[README.md new.txt*] / hello there'
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;keyof:New file;text:ireplaced;key:Escape;key:cmd+S;text:run.log;key:Return;dump" "$W/README.md" 2>&1 >/dev/null | tail -1)
got="$got / $(cat "$W/run.log" 2>/dev/null)"
check "saving under a name that is there writes over it" '>[README.md run.log*] / replaced'
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;keyof:New file;text:ikept;key:Escape;key:cmd+S;text:x.txt;key:Escape;dump" "$W/README.md" 2>&1 >/dev/null | tail -1)
got="$got / $(ls "$W/x.txt" 2>/dev/null)"
check "Esc: no name, nothing saved" '>[README.md untitled*] / '

# Choosing the folder to work in.
run "find:folder;finddump"
check "a folder to work in: folders only, and this one first" "folder [$W/] 6 : >./ ../ build/ docs/ node_modules/ src/"
run "find:folder;text:sr;key:Return;key:Return;key:cmd+P;text:.;finddump"
check "going into a folder and taking it makes it the working directory" 'names [.] 3 of 3 : >deep/inner.txt helper.hpp main.cpp'

# With Vim's modes off it is all the same.
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
run "key:cmd+P;text:notes;key:Return;dump"
check "modal = false: Cmd+P and Enter" '>[README.md notes.md*]'

# Cmd+F without modes: the box that finds in the file, the word at the
# cursor in it, selected; Enter the next match, Shift+Enter the one before.
runf() {
    printf 'one two one\ntwo one\n' >"$dir/r.txt"
    got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "frame;$1" "$dir/r.txt" 2>&1 >/dev/null | tail -1)
}
runf "key:cmd+F;finddump"
check "Cmd+F: the word at the cursor, its match chosen, and how many there are" 'file [one] 1 of 3 at 0-3'
runf "key:cmd+F;key:Return;finddump"
check "Enter: the next" 'file [one] 2 of 3 at 8-11'
runf "key:cmd+F;key:shift+Return;finddump"
check "Shift+Enter: the one before, round the top" 'file [one] 3 of 3 at 16-19'
runf "key:cmd+F;text:tw;finddump"
check "typing takes the place of what was there" 'file [tw] 1 of 2 at 4-6'
runf "key:cmd+F;text:ONE;finddump"
check "a capital typed is a capital found" 'file [ONE] - of 0'
runf "key:cmd+F;key:alt+C;text:one;key:alt+C;finddump"
check "Option+C: case as typed, and off again" 'file [one] 1 of 3 at 0-3'
runf "key:cmd+F;key:alt+R;text:o.e\\|t.o;finddump"
check "Option+R: a regular expression" 'file [o.e\|t.o] .* 1 of 5 at 0-3'
runf "key:cmd+F;key:alt+R;text:\\(;finddump"
check "and one that is not says why" 'file [\(] .* (E54: Unmatched \()'
runf "key:cmd+F;key:Return;key:Escape;keyof:Find the next match;keyof:Find the next match;vimdump"
check "Cmd+G goes on with it after the box has gone" 'NORMAL |  | 1:4 | one two one'
runf "key:cmd+F;text:two;key:Tab;text:2;key:Return;finddump;save"
got="$got | $(tr '\n' '/' <"$dir/r.txt")"
check "Tab: Replace with; Enter replaces the one chosen and chooses the next" 'file [two] with> [2] 1 of 1 at 10-13 | one 2 one/two one/'
runf "key:cmd+alt+F;key:Tab;text:1;key:cmd+Return;save"
got=$(tr '\n' '/' <"$dir/r.txt")
check "Cmd+Option+F, then Cmd+Enter: all of them" '1 two 1/two 1/'
runf "key:cmd+alt+F;key:alt+R;text:\\(o\\)\\(n\\)e;key:Tab;text:\\2\\1&;key:cmd+Return;save"
got=$(tr '\n' '/' <"$dir/r.txt")
check "a regular expression's groups, and & for the match" 'noone two noone/two noone/'
runf "key:cmd+alt+F;key:Tab;text:1;key:cmd+Return;key:Escape;key:cmd+Z;save"
got=$(tr '\n' '/' <"$dir/r.txt")
check "all of them are one undo step" 'one two one/two one/'

# Search everywhere: files, commands and settings in one box, each kind
# under its heading, a few of each.
run "key:cmd+K;finddump"
check "Cmd+K with nothing typed: the files open and opened lately" 'everywhere [] 1 : Open: >README.md'
run "key:cmd+K;text:help;wait:200;frame;finddump"
check "a file by its name" 'everywhere [help] 1 : Files: >src/helper.hpp'
run "key:cmd+K;text:wid;wait:200;frame;finddump"
check "each kind under its heading" 'everywhere [wid] 5 : Commands: >Make the split wider Settings: editor.tab_width=4 editor.indent_width=0 panel.width=32 window.width=1100'
run "key:cmd+K;text:line num;finddump"
check "a setting, with its value" 'everywhere [line num] 1 : Settings: >editor.line_numbers=true'
run "key:cmd+K;text:tot;finddump"
check "commands and settings by the starts of their words, not any letters in order" 'everywhere [tot] 6 : Commands: >Reconnect to the servers that dropped Go to the next hit of Search in files Go to the previous hit of Search in files Go to the next tab Go to the previous tab Go to the last tab'
run "key:cmd+K;text:qqqzzz;wait:200;frame;finddump"
check "nothing" 'everywhere [qqqzzz] 0 :'
run "key:cmd+K;text:wid;wait:200;frame;key:Tab;finddump"
check "Tab goes to the next kind" 'everywhere [wid] 5 : Commands: Make the split wider Settings: >editor.tab_width=4 editor.indent_width=0 panel.width=32 window.width=1100'
run "key:cmd+K;text:help;key:Return;dump"
check "Enter opens a file (the files waited for)" '>[README.md helper.hpp*]'
run "key:cmd+K;text:gofile;key:Return;finddump"
check "runs a command (Go to file)" 'names [] 1 of 6 : Open: >README.md'
run "key:cmd+K;text:line num;key:Return;text:false;vimdump"
check "and a setting is added to init.lua, its value selected" 'NORMAL |  | 4:37 | blit.opt.editor.line_numbers = false'
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n-- blit.opt.editor.tab_width = 4   -- columns\nblit.opt.editor.modal = false -- "x"\n' >"$dir/tmpl.lua"
BLIT_INIT="$dir/tmpl.lua"
run "key:cmd+K;text:tab width;key:Return;text:8;vimdump"
check "the template's line of it, a comment, made a setting" 'NORMAL |  | 3:30 | blit.opt.editor.tab_width = 8   -- columns'
run "key:cmd+K;text:modal;key:Return;text:true;vimdump"
check "the line that sets it" 'NORMAL |  | 4:29 | blit.opt.editor.modal = true -- "x"'
BLIT_INIT="$dir/init.lua"

[ -n "$print" ] || [ $failed = 1 ] || echo "find ok"
exit $failed
