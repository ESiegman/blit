#!/bin/sh
# The settings screen: every setting with its value, changed in place and
# written into init.lua as lines, through scripted keys.
#
#   tests/settings.sh path/to/blit            run the cases
#   tests/settings.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/settings.sh path/to/blit [--print]}
print=${2:-}
src=$(cd "$(dirname "$0")/.." && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
export BLIT_INIT="$dir/init.lua"
base='blit.opt.lsp.enabled = false
blit.opt.editor.modal = false
blit.opt.git.enabled = false'
echo hello >"$dir/a.txt"
failed=0
# check NAME INIT SCRIPT LINE [INIT-AFTER]: init.lua made as INIT, the
# script run, the settings tab's line under the cursor then as LINE (spaces
# squeezed), and init.lua's lines with blit.opt in them as INIT-AFTER.
check() {
    printf '%s\n' "$2" >"$BLIT_INIT"
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "$3;frame;settingsdump" "$dir/a.txt" 2>&1 >/dev/null | grep '^settings:\|^no settings' | tail -1 | tr -s ' ' | sed 's/^settings: [0-9]* items, line: //')
    after=$(grep 'blit\.opt\.' "$BLIT_INIT" | grep -v 'lsp.enabled = false\|editor.modal = false\|git.enabled = false' | tr -s ' ' | tr '\n' '|')
    if [ -n "$print" ]; then
        printf '%s\n    %s\n    %s\n' "$1" "$got" "$after"
        return
    fi
    if [ "$got" != "$4" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$4" "$got"
        failed=1
    fi
    if [ "${5+set}" = set ] && [ "$after" != "$5" ]; then
        printf 'FAIL: %s (init.lua)\n    expected %s\n    got      %s\n' "$1" "$5" "$after"
        failed=1
    fi
}
check "it opens on the first setting" "$base" "settingsopen" \
    'font.family "" A family name ("JetBrains Mono") or the path of a font file.' ""
check "Enter flips a switch, written into init.lua" "$base" "settingsopen:font.ligatures;key:Return" \
    'font.ligatures true draw -> != => <= and friends joined, if the font can' "blit.opt.font.ligatures = true|"
check "flipped back: the line changed, not another added" "$base
blit.opt.font.ligatures = true" "settingsopen:font.ligatures;key:Space" \
    'font.ligatures false draw -> != => <= and friends joined, if the font can' "blit.opt.font.ligatures = false|"
check "a number typed" "$base" "settingsopen:font.size;key:Return;key:Backspace;key:Backspace;text:16;key:Return" \
    'font.size 16 points' "blit.opt.font.size = 16|"
check "not a number: nothing written" "$base" "settingsopen:font.size;key:Return;text:x;key:Return" \
    'font.size 13 points' ""
check "a list typed, with commas" "$base" "settingsopen:font.features;key:Return;text:ss01, zero;key:Return" \
    'font.features { "ss01", "zero" } OpenType features, for example { "ss01", "zero" }' 'blit.opt.font.features = { "ss01", "zero" }|'
check "Delete: back to the default, the line commented out" "$base
blit.opt.font.size = 16" "settingsopen:font.size;key:Delete" \
    'font.size 13 points' "-- blit.opt.font.size = 16|"
# init.lua written by hand, in less usual ways: a table over several lines
# (all of it changed, or commented out), two settings on one line (the
# other kept), a setting inside a long comment (which sets nothing).
# whole NAME EXPECTED: init.lua after the base's lines, its lines joined by "|".
whole() {
    got=$(tail -n +4 "$BLIT_INIT" | tr '\n' '|')
    if [ -z "$print" ] && [ "$got" != "$2" ]; then
        printf 'FAIL: %s (init.lua)\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
features='blit.opt.font.features = {
    "ss01",  -- stylistic set 1
}'
check "a table over several lines, changed: all of it" "$base
$features
blit.opt.font.size = 15" "settingsopen:font.features;key:Return;key:Backspace;key:Backspace;key:Backspace;key:Backspace;text:zero;key:Return" \
    'font.features { "zero" } OpenType features, for example { "ss01", "zero" }'
whole "a table over several lines, changed" 'blit.opt.font.features = { "zero" }|blit.opt.font.size = 15|'
check "a table over several lines, back to the default: all of it commented out" "$base
$features
blit.opt.font.size = 15" "settingsopen:font.features;key:Delete" \
    'font.features {} OpenType features, for example { "ss01", "zero" }'
whole "a table over several lines, commented out" '-- blit.opt.font.features = {|    -- "ss01",  -- stylistic set 1|-- }|blit.opt.font.size = 15|'
check "two settings on a line: the other kept" "$base
blit.opt.font.size = 16; blit.opt.font.ligatures = true" "settingsopen:font.size;key:Return;key:Backspace;key:Backspace;text:18;key:Return" \
    'font.size 18 points' "blit.opt.font.size = 18; blit.opt.font.ligatures = true|"
check "two settings on a line, one back to the default: the other kept, on a line of its own" "$base
blit.opt.font.size = 16; blit.opt.font.ligatures = true" "settingsopen:font.size;key:Delete;settingsopen:font.ligatures" \
    'font.ligatures true draw -> != => <= and friends joined, if the font can' "-- blit.opt.font.size = 16;|blit.opt.font.ligatures = true|"
check "a setting inside a long comment: left there, the line added" "$base
--[[
blit.opt.font.size = 20
]]" "settingsopen:font.size;key:Return;key:Backspace;key:Backspace;text:16;key:Return" \
    'font.size 16 points' "blit.opt.font.size = 20|blit.opt.font.size = 16|"
# The template as init.lua: its commented-out line taken in, its comment
# kept in its column.
tmpl=$(cat "$src/init.example.lua")
check "the template's line taken in, its comment kept" "$tmpl
$base" "settingsopen:font.ligatures;key:Return" \
    'font.ligatures true draw -> != => <= and friends joined, if the font can'
if [ -z "$print" ]; then
    line=$(grep -n 'font.ligatures' "$BLIT_INIT")
    case "$line" in
        *":blit.opt.font.ligatures = true         -- draw -> != => <= and friends joined, if the font can") ;;
        *) printf 'FAIL: the template line\n    got %s\n' "$line"; failed=1 ;;
    esac
    [ "$(grep -c 'font.ligatures' "$BLIT_INIT")" = 1 ] || { echo "FAIL: the template line: another added"; failed=1; }
fi
# init.lua open with changes not saved: the line goes there, not saved.
printf '%s\n' "$base" >"$BLIT_INIT"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "open:$BLIT_INIT;key:cmd+Down;text:-- mine;settingsopen:font.ligatures;key:Return;frame" "$dir/a.txt" 2>&1 >/dev/null)
if [ -z "$print" ] && grep -q 'font.ligatures' "$BLIT_INIT"; then
    echo "FAIL: init.lua with changes not saved was saved"
    failed=1
fi
if [ -z "$print" ] && ! printf '%s' "$got" | grep -q 'changes not saved'; then
    printf 'FAIL: init.lua with changes not saved: not said\n    %s\n' "$(printf '%s' "$got" | tail -3)"
    failed=1
fi
check "after Enter, the next key is still the next key (a search here)" "$base
blit.opt.editor.modal = true" "settingsopen:font.ligatures;key:Return;text:/font.size;key:Return" \
    'font.size 13 points'
check "Cmd+, opens it" "$base" "key:cmd+," \
    'font.family "" A family name ("JetBrains Mono") or the path of a font file.'
exit $failed
