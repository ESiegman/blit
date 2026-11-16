#!/bin/sh
# The keyboard: the command search (every command, by name or by keys),
# and the keys for what had none (moving tabs, resizing splits). Every
# command is run from the search once, each in a run of its own: none may
# fail or crash.
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
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
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
run() {
    got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "workdir:$dir/work;$1" "$dir/work/notes.txt" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | tail -1)
}

run "key:cmd+shift+p;text:split right;finddump"
check "the search: commands by name, with their keys, the best first" 'commands [split right] 4 : >Split to the right (Cmd+\) Go to the split on the right (Cmd+Option+Right) Move the tab to the split on the right (Cmd+Option+Shift+Right) Make the split wider (Cmd+Ctrl+Right)'
run "key:cmd+shift+p;text:cmd+b;finddump"
check "keys typed: the command they are" 'commands [cmd+b] 1 : >Show or hide the side panel (Cmd+B)'
run "key:cmd+shift+p;key:Tab;key:ctrl+Tab;finddump"
check "Tab, then keys pressed: looked up" 'commands [Ctrl+Tab] 1 : >Go to the next tab (Ctrl+Tab  Cmd+Shift+])'
run "key:cmd+shift+p;text:split below;key:Return;frame;dump"
check "a command run from the search: as its keys would" 'V([notes.txt*] / >[untitled*])0.50'
run 'key:cmd+\;key:cmd+ctrl+right;key:cmd+ctrl+right;frame;dump'
check "the split wider (two twentieths)" 'H([notes.txt*] | >[untitled*])0.40'
run 'key:cmd+t;key:cmd+t;key:ctrl+shift+pageup;frame;dump'
check "the tab one place left" '>[notes.txt untitled* untitled]'
run 'key:cmd+t;key:cmd+alt+shift+right;frame;dump;key:cmd+alt+shift+left;frame;dump'
check "the tab into a split of its own, and back" '>[notes.txt untitled*]'
run 'key:cmd+t;key:cmd+alt+shift+down;frame;dump'
check "the tab to a new split below" 'V([notes.txt*] / >[untitled*])0.50'
run 'key:cmd+shift+o;finddump'
check "a folder chosen by its path" 'folder [@/work/] 2 : >./ ../'

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
