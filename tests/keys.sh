#!/bin/sh
# The keyboard: the command search (every command, by name or by keys),
# the keys for what had none (moving tabs, resizing splits), and Back and
# Forward through the places jumps started from. Every command is run from
# the search once, each in a run of its own: none may fail or crash.
#
#   tests/keys.sh path/to/blit            run the cases
#   tests/keys.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/keys.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
mkdir -p "$dir/work"
printf 'notes\n' >"$dir/work/notes.txt"
i=0
while [ $i -lt 60 ]; do
    echo "line $((i + 1)) of long"
    i=$((i + 1))
done >"$dir/work/long.txt"
printf 'one\ntwo\nthe word\n' >"$dir/work/short.txt"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/modal.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH=/usr/bin/false
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
# run SCRIPT [FILE]: the last line the run printed. INIT, if set, is the
# init.lua for this run only.
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "workdir:$dir/work;$1" "${2:-$dir/work/notes.txt}" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | tail -1)
    INIT=
}

# The keys as the commands' list writes them: a Mac's, or those of a system
# where Ctrl is the editor's key (README.md, "Keys without Cmd").
if [ "$(uname -s)" = Darwin ]; then
    split_keys='Split to the right (Cmd+\) Go to the split on the right (Cmd+Option+Right) Move the tab to the split on the right (Cmd+Option+Shift+Right) Make the split wider (Cmd+Ctrl+Right)'
    wider=cmd+ctrl+Right split_count=4
    panel_typed=cmd+b panel_keys='Show or hide the side panel (Cmd+B)'
    panel_map='<D-b>' panel_chord='Cmd+B'
    next_keys='Go to the next tab (Ctrl+Tab  Cmd+Shift+])'
else
    split_keys='Split to the right (Ctrl+\) Go to the split on the right (Ctrl+Alt+Right) Move the tab to the split on the right (Ctrl+Alt+Shift+Right)'  # (the split wider: no Right in its keys there)
    split_count=3
    wider=alt+shift+.  # (Alt+Shift+Right expands the selection there)
    panel_typed=ctrl+b panel_keys='Show or hide the side panel (Ctrl+B  Alt+B)'
    panel_map='<C-b>' panel_chord='Ctrl+B'
    next_keys='Go to the next tab (Ctrl+Tab  Ctrl+Shift+]  Ctrl+PageDown)'
fi
run "key:cmd+shift+p;text:split right;finddump"
check "the search: commands by name, with their keys, the best first" "commands [split right] $split_count : >$split_keys"
run "key:cmd+shift+p;text:$panel_typed;finddump"
check "keys typed: the command they are" "commands [$panel_typed] 1 : >$panel_keys"
run "key:cmd+shift+p;key:alt+K;key:ctrl+Tab;finddump"
check "Option+K, then keys pressed: looked up" "commands [Ctrl+Tab] 1 : >$next_keys"
run "key:cmd+shift+p;key:alt+K;key:alt+K;text:split right;finddump"
check "and Option+K again: back to typing" "commands [split right] $split_count : >$split_keys"
run "key:cmd+shift+p;key:Tab;key:ctrl+Tab;finddump"
case $got in "commands [] "*) got="nothing looked up" ;; esac
check "Tab does not look keys up" "nothing looked up"
# A script's mapping that takes a command's key: keys looked up name it
# first, and Enter runs it, as the key would.
mapped="frame;lua:blit.map('n','$panel_map',function() blit.notify('mine') end, { desc = 'mine' })"
INIT="$dir/modal.lua"
run "$mapped;key:cmd+shift+p;key:alt+K;key:cmd+B;finddump"
check "a mapping that takes a command's key: named first" "commands [$panel_chord] 2 : >Mapping $panel_map (n): mine ($panel_chord) $panel_keys"
INIT="$dir/modal.lua"
run "$mapped;key:cmd+shift+p;text:$panel_typed;finddump"
check "and the same for its keys typed" "commands [$panel_typed] 2 : >Mapping $panel_map (n): mine ($panel_chord) $panel_keys"
INIT="$dir/modal.lua"
run "$mapped;key:cmd+shift+p;key:alt+K;key:cmd+B;key:Return;frame"
check "Enter on it runs the mapping" 'blit: mine'
run "key:cmd+shift+p;text:split below;key:Return;frame;dump"
check "a command run from the search: as its keys would" 'V([notes.txt*] / >[untitled*])0.50'
run 'key:cmd+\;key:Escape;keyof:Make the split wider;keyof:Make the split wider;frame;dump'
check "the split wider (two twentieths)" 'H([notes.txt*] | >[untitled*])0.40'
run "key:cmd+\\;key:Escape;key:$wider;key:$wider;frame;dump"
check "and by its keys" 'H([notes.txt*] | >[untitled*])0.40'
run "frame;keyclashes"
check "no keys are two commands'" 'no clashes'
run 'key:cmd+t;key:cmd+t;key:ctrl+shift+pageup;frame;dump'
check "the tab one place left" '>[notes.txt untitled* untitled]'
run "open:$dir/work/long.txt;open:$dir/work/short.txt;key:cmd+alt+shift+[;key:cmd+alt+shift+[;frame;dump"
check "the tab left with keys a MacBook has (Cmd+Option+Shift+[)" '>[short.txt* notes.txt long.txt]'
run "open:$dir/work/long.txt;open:$dir/work/short.txt;key:cmd+1;key:cmd+alt+shift+];frame;dump;key:cmd+shift+];frame;dump"
check "and right (Cmd+Option+Shift+]), and Cmd+Shift+] still goes to the next tab" '>[long.txt notes.txt short.txt*]'
run 'key:cmd+t;key:cmd+alt+shift+right;frame;dump;key:cmd+alt+shift+left;frame;dump'
check "the tab into a split of its own, and back" '>[notes.txt untitled*]'
run 'key:cmd+t;key:cmd+alt+shift+down;frame;dump'
check "the tab to a new split below" 'V([notes.txt*] / >[untitled*])0.50'
run 'key:cmd+shift+o;finddump'
check "a folder chosen by its path" 'folder [@/work/] 2 : >./ ../'

# Back (Ctrl+-) and forward (Ctrl+Shift+-) through one list of the places
# jumps started from: Go to file, Search in files, Vim's G and marks, in
# any mode, with no language server.
run "key:cmd+p;text:long.txt;findwait;key:Return;key:cmd+p;text::40;key:Return;frame;jumpdump"
check "Go to file and its :line go into the list" 'notes.txt:1 long.txt:1 >'
run "key:cmd+p;text:long.txt;findwait;key:Return;key:cmd+p;text::40;key:Return;keyof:Go back;keyof:Go back;frame;statusdump"
check "Ctrl+- back to where each began, the file before too" 'notes.txt | Ln 1, Col 1 Tabs LF Plain text'
run "key:cmd+p;text:long.txt;findwait;key:Return;key:cmd+p;text::40;key:Return;keyof:Go back;keyof:Go back;keyof:Go forward;keyof:Go forward;frame;jumpdump;statusdump"
check "Ctrl+Shift+- forward again, to where the walk back began" 'long.txt | Ln 40, Col 1 Tabs LF Plain text'
run "key:cmd+shift+f;text:the word;findwait;key:Return;keyof:Go back;frame;statusdump"
check "a line found by Search in files, and back" 'notes.txt | Ln 1, Col 1 Tabs LF Plain text'
INIT="$dir/modal.lua"
run "vim:G;key:cmd+p;text:short.txt;findwait;key:Return;vim:jjmagg'a;jumpdump" "$dir/work/long.txt"
check "Vim's long moves go in too: G, a mark" 'long.txt:1 long.txt:60 short.txt:3 short.txt:1 >'
INIT="$dir/modal.lua"
run "vim:G;key:cmd+p;text:short.txt;findwait;key:Return;vim:jjmagg'a;vim:<C-o>;vim:<C-o>;vim:<C-o>;vimdump;vim:<C-i>;vim:<C-i>;frame;vimdump" "$dir/work/long.txt"
check "Ctrl-O walks the same list, across files, and Ctrl-I comes back" 'NORMAL |  | 1:1 | one'
INIT="$dir/modal.lua"
run "vim:Ggg;vim:i;keyof:Go back;frame;vimdump" "$dir/work/long.txt"
check "Ctrl+- in Insert mode too, which it stays in" 'INSERT | -- INSERT -- | 60:1 | line 60 of long'
INIT="$dir/modal.lua"
run "vim:G;key:cmd+p;text:short.txt;findwait;key:Return;vim:i;keyof:Go back;frame;vimdump" "$dir/work/long.txt"
check "and to another file from Insert mode (Vim starts there in Normal mode)" 'NORMAL |  | 60:1 | line 60 of long'

# Every command, from the search (Quit and Close the tab end the run; the rest leave it going).
"$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "commands" 2>&1 >/dev/null | cut -f1 | sed -e 's/;.*//' >"$dir/commands"
n=$(wc -l <"$dir/commands" | tr -d ' ')
bad=0
while IFS= read -r c; do
    out=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "workdir:$dir/work;command:$c;frame;frame;key:Escape;frame" "$dir/work/notes.txt" 2>&1 >/dev/null)
    code=$?
    case "$out" in *"command: no"*) code=99 ;; esac
    if [ $code -ne 0 ]; then
        printf 'FAIL: command "%s" (exit %s): %s\n' "$c" "$code" "$(printf '%s' "$out" | tail -2)"
        bad=$((bad + 1))
    fi
done <"$dir/commands"
got="$n commands, $bad failed"
case "$got" in [6-9][0-9]" commands, 0 failed" | [1-9][0-9][0-9]" commands, 0 failed") got="every command runs" ;; esac
check "every command runs from the search" "every command runs"

[ -n "$print" ] || [ $failed = 1 ] || echo "keys ok"
exit $failed
