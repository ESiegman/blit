#!/bin/sh
# Pictures: an image file in its tab (what it says of it, its pixels where
# they should be, a see-through part over a checkerboard, zoomed by keys and
# a pinch, moved by keys and by dragging), a very large one read smaller,
# one that is not a picture, one whose file changes, one on another machine
# (tests/fake-ssh.sh stands in for ssh), and pictures in the Markdown
# preview (tests/images/pics.md), one of which a click opens.
#
#   tests/images.sh path/to/blit            run the cases
#   tests/images.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/images.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent"
pics="$here/images"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
join() { awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }'; }
# run SIZE SCRIPT [FILE]: what the run printed (where the picture was drawn
# left out, unless WHERE is set), its lines joined by " ;; ".
WHERE=
run() {
    got=$("$blit" --offscreen "$1" --shot "$dir/shot.bmp" --script "$2" ${3:+"$3"} 2>&1 >/dev/null | grep -v '^\[' |
        if [ -n "$WHERE" ]; then cat; else sed -e 's/ at [-0-9,]*$//'; fi | join)
    WHERE=
}

# The tab: what it is, fitted (no larger than its own size).
run 800x600 "frame;imagewait;frame;imagedump" "$pics/quad.png"
check "an image file, in its tab" "quad.png: ready PNG 64x48 read 64x48 clear zoom 100% middle 0.50,0.50"

# Its pixels: each quarter's colour in the middle of it, and the clear
# quarter a checkerboard (two squares side by side differ).
WHERE=1 run 800x600 "frame;imagewait;frame;imagedump" "$pics/quad.png"
set -- $(printf '%s\n' "$got" | sed -e 's/.* at //' -e 's/,/ /g')
x0=$1 y0=$2 x1=$3 y1=$4
qx=$(((x1 - x0) / 4)) qy=$(((y1 - y0) / 4))
run 800x600 "frame;imagewait;frame;pixel:$((x0 + qx)),$((y0 + qy));pixel:$((x1 - qx)),$((y0 + qy));pixel:$((x0 + qx)),$((y1 - qy))" "$pics/quad.png"
got=$(printf '%s' "$got" | sed -e 's/pixel [0-9,]*: //g')
check "its pixels: red, green and blue quarters" "255 0 0 ;; 0 255 0 ;; 0 0 255"
run 800x600 "frame;imagewait;frame;pixel:$((x1 - qx - 16)),$((y1 - qy));pixel:$((x1 - qx)),$((y1 - qy))" "$pics/quad.png"
a=$(printf '%s' "$got" | sed -e 's/ ;; .*//' -e 's/.*: //') b=$(printf '%s' "$got" | sed -e 's/.* ;; //' -e 's/.*: //')
got=$([ "$a" != "$b" ] && [ "$a" != "0 0 0" ] && echo checkerboard || echo "$a / $b")
check "the clear quarter: a checkerboard behind it" "checkerboard"

# Zoomed: + and -, a pinch (about the mouse), 0 to fit it again.
run 800x600 "frame;imagewait;frame;key:=;frame;imagedump;key:-;frame;imagedump;mouse:200,150;pinch:2;frame;imagedump;key:0;frame;imagedump" "$pics/quad.png"
check "zoomed by keys and a pinch, and fitted again" "quad.png: ready PNG 64x48 read 64x48 clear zoom 125% middle 0.50,0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 100% middle 0.50,0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 100% middle 0.50,0.50"

# A picture wider than the GPU is asked to hold is read smaller; at its
# own size (1) it is moved by the keys and by dragging it.
run 800x600 "frame;imagewait;frame;imagedump;key:1;frame;imagedump;key:Right;frame;imagedump;click:200,150;mouse:150,150;release;frame;imagedump" "$pics/wide.png"
check "a very wide picture: read smaller, moved" "wide.png: ready PNG 5000x20 read 4096x16 zoom 8% middle 0.50,0.50 ;; wide.png: ready PNG 5000x20 read 4096x16 zoom 100% middle 0.50,0.50 ;; wide.png: ready PNG 5000x20 read 4096x16 zoom 100% middle 0.51,0.50 ;; wide.png: ready PNG 5000x20 read 4096x16 zoom 100% middle 0.52,0.50"

# Not a picture, and no file at all: the tab says so.
run 800x600 "frame;imagewait;frame;imagedump" "$pics/broken.png"
check "a file that is not a picture" "broken.png: failed (not a picture this system can read)"
run 800x600 "frame;imagewait;frame;imagedump" "$dir/gone.png"
check "a picture that is not there" "gone.png: failed (there is no such file)"

# Its file changes (written by another program): read again when blit comes back to the front.
cp "$pics/quad.png" "$dir/live.png"
run 800x600 "frame;imagewait;frame;imagedump;lua:blit.fs.write('$dir/live.png', blit.fs.read('$pics/wide.png'));reload;imagewait;frame;imagedump" "$dir/live.png"
check "its file written again: read again" "live.png: ready PNG 64x48 read 64x48 clear zoom 100% middle 0.50,0.50 ;; live.png: ready PNG 5000x20 read 4096x16 zoom 8% middle 0.50,0.50"

# The Markdown preview: pictures beside the file drawn (one in the middle,
# at the width its <img> asks for), one on the web and one not there as
# their text, one in a sentence as its text.
run 1400x1000 "frame;mdpreview;frame;imagewait;frame;mddump" "$pics/pics.md"
got=$(printf '%s' "$got" | sed -e 's/\( ;; \)*$//' -e 's/\( ;; \)\{2,\}/ ;; /g')
check "pictures in the Markdown preview" "Pictures ;; ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ ;; picture: quad.png 8x4 ;; Text with ▣ an inline one in it. ;; picture: quad.png 4x2 ;; ▣ on the web ;; ▣ not there ;; picture: quad.png 8x4"

# A click on a picture there opens it, in a tab of its own.
run 1400x1000 "frame;mdpreview;frame;imagewait;frame;click:400,95;frame;imagewait;frame;dump;imagedump" "$pics/pics.md"
check "a picture in the preview, clicked: its file opened" "H([pics.md*] | >[Preview: pics.md quad.png*])0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 100% middle 0.50,0.50"

# On another machine: read there, by blit's agent.
T="$dir/there"
mkdir -p "$T/work"
cp "$pics/quad.png" "$T/work/"
BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T" run 800x600 "remotewait;frame;imagewait;frame;imagedump" "ssh://box$T/work/quad.png"
check "a picture on another machine" "quad.png: ready PNG 64x48 read 64x48 clear zoom 100% middle 0.50,0.50"

[ -n "$print" ] || [ $failed = 1 ] || echo "images ok"
exit $failed
