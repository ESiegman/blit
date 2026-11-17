#!/bin/sh
# Characters made of more than one code point, and characters of other
# widths: a letter with combining marks, emoji joined by zero width joiners,
# flags, skin tones, variation selectors, a spacing mark. Each is one
# character to the cursor (the arrows, Backspace and Delete, words, the
# lines above and below) and takes the cells it should: none for a mark,
# which is drawn over its letter, or for what joins an emoji.
#
#   tests/unicode.sh path/to/blit            run the cases
#   tests/unicode.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/unicode.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
export BLIT_RECENT_FILE="$dir/recent"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"

E=$(printf 'e\314\201')                                                        # e, and a combining acute over it
MARK=$(printf '\314\201')                                                      # the acute alone
FAMILY=$(printf '\360\237\221\250\342\200\215\360\237\221\251\342\200\215\360\237\221\247')  # man, ZWJ, woman, ZWJ, girl
MAN=$(printf '\360\237\221\250')
FLAG=$(printf '\360\237\207\257\360\237\207\265')                              # J and P: Japan's flag
J=$(printf '\360\237\207\257')
FR=$(printf '\360\237\207\253\360\237\207\267')
THUMB=$(printf '\360\237\221\215\360\237\217\275')                              # thumbs up, a skin tone
THUMB1=$(printf '\360\237\221\215')
CHECK=$(printf '\342\234\205')                                                 # U+2705, an emoji two cells wide
VS16=$(printf '\357\270\217')
KA=$(printf '\340\244\225\340\244\276')                                        # Devanagari ka, and the vowel sign aa (a spacing mark)
ZWSP=$(printf '\342\200\213')
WIDE=$(printf '\342\234\205\342\255\220\342\214\232')                          # U+2705 U+2B50 U+231A, two cells each

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run TEXT SCRIPT: what the script prints, on one line ("blit: " and tabs
# taken out). $AT in a script prints where the cursor is (line and column,
# the column in bytes from 1); $LINE prints line 1.
AT='lua:print(blit.cursor())'
LINE='lua:print(blit.lines(0, 1, 1)[1])'
run() {
    printf '%s\n' "$1" >"$dir/u.txt"
    got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;$2" "$dir/u.txt" 2>&1 >/dev/null | grep '^blit: ' | sed -e 's/^blit: //' | tr '\t' ':' | tr '\n' ' ' | sed -e 's/ $//')
}
# The status bar's "Ln N, Col M" (M is the display column, from 1).
col() {
    printf '%s\n' "$1" >"$dir/u.txt"
    got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "frame;$2;frame;statusdump" "$dir/u.txt" 2>&1 >/dev/null | grep -o 'Ln [0-9]*, Col [0-9]*')
}

# The arrows step over a letter and its mark, an emoji sequence, a flag, a
# skin tone, a spacing mark; not over a zero width space, which is a
# character of its own (a place on either side of it).
run "caf$E x" "key:Right;key:Right;key:Right;$AT;key:Right;$AT;key:Right;$AT;key:Left;$AT;key:Left;$AT"
check "Right and Left over e and a combining acute" "1:4 1:7 1:8 1:7 1:4"
run "$FAMILY!" "key:Right;$AT;key:Right;$AT;key:Left;$AT;key:Left;$AT"
check "an emoji family (joined by ZWJ) is one character" "1:19 1:20 1:19 1:1"
run "$FLAG$FR." "key:Right;$AT;key:Right;$AT;key:Right;$AT;key:Left;$AT;key:Left;$AT"
check "two flags, a pair of regional indicators each" "1:9 1:17 1:18 1:17 1:9"
run "$THUMB." "key:Right;$AT;key:Left;$AT"
check "a skin tone goes with its emoji" "1:9 1:1"
run "$CHECK$VS16." "key:Right;$AT"
check "a variation selector goes with its character" "1:7"
run "$KA." "key:Right;$AT"
check "a spacing mark goes with its letter" "1:7"
run "a${ZWSP}b" "key:Right;$AT;key:Right;$AT;key:Right;$AT"
check "a zero width space is a character of its own" "1:2 1:5 1:6"

# Backspace and Delete take the whole character.
run "caf$E x" "key:End;key:Left;key:Left;key:Backspace;$LINE;$AT"
check "Backspace takes the letter and its mark" "caf x 1:4"
run "caf$E x" "key:Right;key:Right;key:Right;key:Delete;$LINE"
check "Delete takes the letter and its mark" "caf x"
run "a$FAMILY" "key:End;key:Backspace;$LINE"
check "Backspace takes an emoji family whole" "a"

# Words: a mark on a space belongs with the space, not with the word after it.
run "x ${MARK}abc" "key:End;key:alt+Left;$AT;key:alt+Left;$AT"
check "Option+Left stops at the word, not inside the space and its mark" "1:5 1:1"
run "x ${MARK}abc" "key:End;key:alt+Backspace;$LINE"
check "Option+Backspace takes the word and leaves the space with its mark" "x $MARK"

# Up and Down land on a character's edge, never inside it.
run "abcdef
$FAMILY!" "key:Right;key:Down;$AT;key:Up;key:Right;key:Down;$AT"
check "Down from over the middle of an emoji family" "2:1 2:19"

# Widths: a mark and a joined emoji take no cells, an emoji two.
col "caf$E|" "key:End"
check "a combining mark takes no cell" "Ln 1, Col 6"
col "$FAMILY!" "key:End"
check "an emoji family takes two cells" "Ln 1, Col 4"
col "$FLAG$FR." "key:End"
check "a flag takes two cells" "Ln 1, Col 6"
col "$WIDE." "key:End"
check "emoji shown as emoji (U+2705, U+2B50, U+231A) take two cells each" "Ln 1, Col 8"
col "$THUMB$CHECK$VS16${ZWSP}x" "key:End"
check "a skin tone, a variation selector and a zero width space take none" "Ln 1, Col 6"

# What is drawn. Frames are compared pixel for pixel: the same file name,
# the cursor where it was.
failed_shot() { printf 'FAIL: %s\n' "$1"; failed=1; }
shot() {
    printf '%s\n' "$2" >"$dir/u.txt"
    "$blit" --offscreen 1200x500 --shot "$dir/$1.bmp" --script "frame;${3:-frame};frame" "$dir/u.txt" >/dev/null 2>&1
    [ -s "$dir/$1.bmp" ] || failed_shot "no frame for $1"
}
same() { cmp -s "$dir/$1.bmp" "$dir/$2.bmp" || failed_shot "$3"; }
differ() { ! cmp -s "$dir/$1.bmp" "$dir/$2.bmp" || failed_shot "$3"; }
digits=0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
shot marked "$E$digits"
shot plain "e$digits"
differ marked plain "the combining acute is not drawn"
shot marked_on "$E$digits" "col:4"
shot plain_on "e$digits" "col:4"
same marked_on plain_on "a combining mark took a cell of its own (the text after it moved)"
shot family "${FAMILY}x"
shot man "${MAN}x"
same family man "an emoji family is not drawn as its first emoji, in two cells"
shot flag "${FLAG}x"
shot j "${J}x"
same flag j "the second half of a flag took cells"
shot thumb "${THUMB}x"
shot thumb1 "${THUMB1}x"
same thumb thumb1 "a skin tone took cells"
shot selected "${CHECK}${VS16}a${ZWSP}b"
shot unselected "${CHECK}ab"
same selected unselected "a variation selector or a zero width space was drawn"

[ -n "$print" ] || [ $failed = 1 ] || echo "unicode ok"
exit $failed
