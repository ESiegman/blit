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
check "Cmd+P lists the files, without what .gitignore and the usual rules leave out" 'names [] 6 of 6 : >.gitignore README.md docs/notes.md src/deep/inner.txt src/helper.hpp src/main.cpp'
run "key:cmd+P;text:main;finddump"
check "what is typed narrows it" 'names [main] 1 of 1 : >src/main.cpp'
run "key:cmd+P;text:hlp;finddump"
check "the letters need not be next to one another" 'names [hlp] 1 of 1 : >src/helper.hpp'
run "key:cmd+P;text:src e;finddump"
check "every word typed has to match; the best match is first" 'names [src e] 2 of 2 : >src/helper.hpp src/deep/inner.txt'
run "key:cmd+P;text:inner;key:Return;dump"
check "Enter opens the one chosen, in a tab" '>[README.md inner.txt*]'
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

# A path, where the system's dialogs used to be.
run "key:cmd+O;finddump"
check "Cmd+O: the working directory as it is on disk, folders first" "open [$W/] 7 : >../ build/ docs/ node_modules/ src/ README.md"
run "key:cmd+O;text:sr;key:Return;finddump"
check "Enter on a folder goes into it" "open [$W/src/] 4 : >../ deep/ helper.hpp main.cpp"
run "key:cmd+O;text:src/he;key:Return;dump"
check "Enter on a file opens it" '>[README.md helper.hpp*]'
run "key:cmd+O;text:do;key:Tab;text:no;key:Tab;finddump"
check "Tab completes the name chosen" "open [$W/docs/notes.md] 1 : >notes.md"
run "key:cmd+O;text:src/;key:Return;finddump"
check "the first row of a folder is the one above it" "open [$W/] 7 : >../ build/ docs/ node_modules/ src/ README.md"
run "key:cmd+O;text:brand_new.txt;finddump;key:Return;dump"
check "a name that is not there opens a new file of that name" '>[README.md brand_new.txt*]'
run "key:cmd+O;text:.g;finddump"
check "names with a dot in front show once a dot is typed" "open [$W/.g] 2 : >.gitignore run.log"
run "key:cmd+O;text:src/deep/;key:alt+Backspace;finddump"
check "Option+Backspace goes a folder back" "open [$W/src/] 4 : >../ deep/ helper.hpp main.cpp"

# Saving a new file asks for its name here.
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;key:cmd+T;text:ihello there;key:Escape;key:cmd+S;finddump" "$W/README.md" 2>&1 >/dev/null | tail -1)
check "Cmd+S in a new file asks for a name, starting in the working directory" "save [$W/] 7 : >../ build/ docs/ node_modules/ src/ README.md"
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;key:cmd+T;text:ihello there;key:Escape;key:cmd+S;text:docs/new.txt;key:Return;dump" "$W/README.md" 2>&1 >/dev/null | tail -1)
got="$got / $(cat "$W/docs/new.txt" 2>/dev/null)"
check "the name typed is the file's from then on, and it is saved" '>[README.md new.txt*] / hello there'
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;key:cmd+T;text:ireplaced;key:Escape;key:cmd+S;text:run.log;key:Return;dump" "$W/README.md" 2>&1 >/dev/null | tail -1)
got="$got / $(cat "$W/run.log" 2>/dev/null)"
check "saving under a name that is there writes over it" '>[README.md run.log*] / replaced'
fresh
got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$W;frame;key:cmd+T;text:ikept;key:Escape;key:cmd+S;text:x.txt;key:Escape;dump" "$W/README.md" 2>&1 >/dev/null | tail -1)
got="$got / $(ls "$W/x.txt" 2>/dev/null)"
check "Esc: no name, nothing saved" '>[README.md untitled*] / '

# Choosing the folder to work in.
run "find:folder;finddump"
check "a folder to work in: folders only, and this one first" "folder [$W/] 6 : >./ ../ build/ docs/ node_modules/ src/"
run "find:folder;text:sr;key:Return;key:Return;key:cmd+P;finddump"
check "going into a folder and taking it makes it the working directory" 'names [] 3 of 3 : >deep/inner.txt helper.hpp main.cpp'

# With Vim's modes off it is all the same.
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
run "key:cmd+P;text:notes;key:Return;dump"
check "modal = false: Cmd+P and Enter" '>[README.md notes.md*]'

[ -n "$print" ] || [ $failed = 1 ] || echo "find ok"
exit $failed
