#!/bin/sh
# Terminals, through the real key and text events: a shell here and one on
# "another machine" (tests/fake-ssh.sh, which runs it here), its screen, its
# size, keys, the alternate screen, copying and pasting (a scripted run's
# own clipboard, never the system's), and the tab going when the shell ends.
#
#   tests/term.sh path/to/blit            run the cases
#   tests/term.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/term.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
T="$dir/there"
mkdir -p "$dir/work" "$T/proj"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
# The same shell everywhere, with a prompt that is the same too.
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: the last line it printed (the folders made here written @).
run() {
    got=$("$blit" --offscreen 1200x600 --shot "$dir/shot.bmp" --script "workdir:$dir/work;$1" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | tail -1)
}

# (The tab is 15 rows of 74 columns at this size: its first frame tells the shell.)
# (What is waited for is what the shell prints, not what is typed: hel''lo, then hello.)
run "term;frame;termwait:\$;text:echo hel''lo;key:Return;termwait:hello;termdump"
check "a shell in the folder being worked in: what it is told, it does" "\$ echo hel''lo|hello|\$ @2,2 15x74"
run "term;frame;termwait:\$;text:basename \$PWD;key:Return;termwait:work;text:stty size;key:Return;termwait:15 74;termdump"
check "it runs in the working folder, and knows the tab's size" '$ basename $PWD|work|$ stty size|15 74|$ @4,2 15x74'
run "term;frame;termwait:\$;text:sleep 30;key:Return;termwait:sleep;key:ctrl+c;text:echo after-\$((1+1));key:Return;termwait:after-2;termdump"
case "$got" in *'after-2|$ @'*) got="interrupted" ;; esac
check "Ctrl+C stops what runs" "interrupted"
run "term;frame;termwait:\$;text:printf '\\033[?1049h\\033[2J%s%s' FULL SCREEN;key:Return;termwait:FULLSCREEN;termdump"
check "the alternate screen (the cursor on the row it was on)" 'FULLSCREEN$ @1,12 15x74'
run "term;frame;termwait:\$;text:printf '\\033[?1049h\\033[2J%s%s' FULL SCREEN;key:Return;termwait:FULLSCREEN;text:printf '\\033[?1049l%s%s' BA CK;key:Return;termwait:BACK;termdump"
case "$got" in *"FULL SCREEN|BACK\$ "*) got="the shell's screen as it was" ;; esac
check "and back from it" "the shell's screen as it was"
run "term;frame;termwait:\$;text:echo copy-m''e;key:Return;termwait:copy-me;click:24,52,2;release;key:cmd+c;key:cmd+v;termwait:\$ copy-me;termdump"
check "a word chosen with a double click, copied, and pasted after the prompt" "\$ echo copy-m''e|copy-me|\$ copy-me @2,9 15x74"
run "term;frame;termwait:\$;text:exit;key:Return;termend;frame;dump"
check "the shell ends: its tab goes" '>[untitled*]'
run "term;frame;termwait:\$;text:exit 3;key:Return;termend;frame;termdump"
case "$got" in *"[exited with code 3]"*" ended 3") got="ended 3, tab kept" ;; esac
check "a shell that ends badly: the tab stays and says so" "ended 3, tab kept"
run "lua:blit.opt.editor.modal = true;term;frame;termwait:\$;text:dd;text:i;text::wq;termwait:ddi:wq;termdump"
check "with Vim's keys on, keys typed go to the shell" '$ ddi:wq @0,8 15x74'

# Ctrl+`: the terminals' split along the bottom (the build's output's too).
# (key:space before typing: a window sends a key before its text, and the
# text Ctrl+` itself might bring is dropped until then. The program says
# "running" once it runs, in the terminal's foreground; what is typed says
# run''ning.)
open="key:ctrl+\`;frame;termwait:\$;key:space"
busy="sh -c 'echo run''ning && exec sleep 30'"
run "$open;dump"
check "Ctrl+\`: a terminal in a split below the text, with the keyboard" 'V([untitled*] / >[Terminal*])0.70'
run "$open;termdump"
check "the hint once, above the shell's first line" 'Ctrl+` back to the file|$ @1,2 3x74'
run "$open;key:ctrl+\`;frame;dump"
check "again: the keyboard back to the text, and the split out of the way" '>[untitled*]'
run "$open;key:ctrl+\`;frame;key:ctrl+\`;frame;dump"
check "and again: the split back as it was" 'V([untitled*] / >[Terminal*])0.70'
run "$open;text:echo hel''lo;key:Return;termwait:hello;key:ctrl+\`;frame;key:ctrl+\`;frame;termdump"
check "with the same terminal, its shell still there" "\$ echo hel''lo|hello|\$ @2,2 3x74"
run "$open;click:300,100;release;frame;key:ctrl+\`;frame;dump"
check "on show with the keyboard in the text: Ctrl+\` gives the terminal the keyboard" 'V([untitled*] / >[Terminal*])0.70'
run "$open;key:ctrl+shift+\`;frame;termwait:\$;dump"
check "Ctrl+Shift+\`: another terminal there" 'V([untitled*] / >[Terminal Terminal*])0.70'
run "$open;text:sleep 1 && exit;key:Return;key:ctrl+\`;wait:2500;termdump;key:ctrl+\`;frame;termwait:\$;dump"
check "a shell that exits while hidden goes; Ctrl+\` then opens a new one" 'V([untitled*] / >[Terminal*])0.70'
run "$open;key:cmd+W;asked"
check "closing a terminal with only its shell: nothing asked" 'nothing asked'
run "$open;text:$busy;key:Return;termwait:running;key:cmd+W;asked"
check "closing one whose program still runs: asked first" "'sleep 30' is still running in this terminal. Close it?"
run "$open;text:$busy;key:Return;termwait:running;key:ctrl+shift+\`;frame;termwait:\$;key:space;text:$busy;key:Return;termwait:running;key:ctrl+\`;key:cmd+Q;asked"
check "quitting: the programs still running counted, hidden ones too" '2 programs are still running in terminals. Quit and end them?'
run "key:Escape;text:i;text:x;$open;text:$busy;key:Return;termwait:running;key:ctrl+\`;key:cmd+Q;asked"
check "and after the files to save" 'Save the changes to a new file before quitting? 1 program is still running in a terminal.'
# (With no working folder chosen, the terminal's folder is found from the file in front, not from the new split.)
printf 'x\n' >"$dir/work/a.txt"
got=$("$blit" --offscreen 1200x600 --shot "$dir/shot.bmp" --script "open:$dir/work/a.txt;$open;text:basename \$PWD;key:Return;termwait:work;dump;termdump" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | tail -2 | tr '\n' ' ')
check "no working folder: Ctrl+\` in the file's folder" 'V([a.txt*] / >[Terminal*])0.70 $ basename $PWD|work|$ @2,2 3x74 '
mkdir -p "$dir/mk"
printf 'all:\n\t@echo built\n' >"$dir/mk/Makefile"
run "workdir:$dir/mk;buildrun;buildwait;key:ctrl+\`;frame;termwait:\$;dump"
check "the build's output and the terminal share the split" 'V([untitled*] / >[make Terminal*])0.70'
run "workdir:$dir/mk;$open;key:ctrl+\`;frame;buildrun;buildwait;dump"
check "a build brings the hidden split back, the keyboard staying in the text" 'V(>[untitled*] / [Terminal make*])0.70'

# On another machine: the connection is made by the terminal itself.
run "workdir:ssh://box$T/proj;term;frame;termwait:\$;text:echo \$(basename \$PWD)-\$(basename \$HOME);key:Return;termwait:proj-;termdump"
check "a terminal on another machine, in the folder there, at home there" '$ echo $(basename $PWD)-$(basename $HOME)|proj-there|$ @2,2 15x74'
run "workdir:ssh://box$T/proj;term;frame;termwait:\$;text:stty size;key:Return;termwait:15 74;termdump"
check "and it knows the tab's size too" '$ stty size|15 74|$ @2,2 15x74'

[ -n "$print" ] || [ $failed = 1 ] || echo "term ok"
exit $failed
