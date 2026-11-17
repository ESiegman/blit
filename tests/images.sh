#!/bin/sh
# Pictures: an image file in its tab (what it says of it, its pixels where
# they should be, a see-through part over a checkerboard, zoomed by keys and
# a pinch, moved by keys and by dragging), a very large one read smaller,
# one that is not a picture, one whose file changes, one on another machine
# (tests/fake-ssh.sh stands in for ssh), and pictures in the Markdown
# preview (tests/images/pics.md), one of which a click opens; animations
# played only while they are on show, and SVG drawn at the size shown. And
# PDFs (tests/images/pages.pdf: a red and blue page, a green one, and one
# turned on its side): pages drawn where they should be, the pages and zoom
# keys, zoomed in far (the part on show drawn again), one that is not a
# PDF, one on another machine; their text found, chosen and copied, their
# links and outline (tests/images/text.pdf), and one locked with a password
# (tests/images/locked.pdf).
#
# The small files there were made by the system's own writers: anim.gif and
# anim.png by ImageIO, locked.pdf by CoreGraphics (password "blit");
# text.pdf, pages.pdf and shapes.svg are written by hand.
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
# The end of a file: Cmd+Down on a Mac, Ctrl+End elsewhere.
case $(uname -s) in Darwin) doc_end=cmd+Down ;; *) doc_end=ctrl+End ;; esac
if "$blit" --offscreen 800x600 --info 2>/dev/null | grep -q '^pictures    none'; then
    echo "images skipped: blit reads no pictures or PDFs on this system yet"
    exit 0
fi

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

# The tab: what it is, fitted (a small one drawn larger by whole steps, to about half the room).
run 800x600 "frame;imagewait;frame;imagedump" "$pics/quad.png"
check "an image file, in its tab" "quad.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50"

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
check "zoomed by keys and a pinch, and fitted again" "quad.png: ready PNG 64x48 read 64x48 clear zoom 250% middle 0.50,0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 400% middle 0.50,0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50"

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
check "its file written again: read again" "live.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50 ;; live.png: ready PNG 5000x20 read 4096x16 zoom 8% middle 0.50,0.50"

# The Markdown preview: pictures beside the file drawn (one in the middle,
# at the width its <img> asks for), one on the web and one not there as
# their text, one in a sentence as its text.
run 1400x1000 "frame;mdpreview;frame;imagewait;frame;mddump" "$pics/pics.md"
got=$(printf '%s' "$got" | sed -e 's/\( ;; \)*$//' -e 's/\( ;; \)\{2,\}/ ;; /g')
check "pictures in the Markdown preview" "Pictures ;; ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ ;; picture: quad.png 8x4 ;; Text with ▣ an inline one in it. ;; picture: quad.png 4x2 ;; ▣ on the web ;; ▣ not there ;; picture: quad.png 8x4"

# A click on a picture there opens it, in a tab of its own.
run 1400x1000 "frame;mdpreview;frame;imagewait;frame;mdwait;click:400,95;frame;imagewait;frame;dump;imagedump" "$pics/pics.md"
check "a picture in the preview, clicked: its file opened" "H([pics.md*] | >[Preview: pics.md quad.png*])0.50 ;; quad.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50"

# A PDF: its pages, fitted to the width of the widest, the first one's colours.
pdf="$pics/pages.pdf"
run 1200x900 "frame;pdfwait;frame;pdfdump" "$pdf"
got=$(printf '%s' "$got" | sed -e 's/, drawn.*//')
check "a PDF, in its tab" "pages.pdf: ready, 3 pages, page 1, zoom 74%"
WHERE=1 run 1200x900 "frame;pdfwait;frame;pdfpage:1" "$pdf"
set -- $(printf '%s\n' "$got" | sed -e 's/.* at //' -e 's/,/ /g')
run 1200x900 "frame;pdfwait;frame;pixel:$(($1 + 40)),$(($2 + 40));pixel:$(($1 + 40)),$((($2 + $4) / 2 + 40))" "$pdf"
got=$(printf '%s' "$got" | sed -e 's/pixel [0-9,]*: //g')
check "its first page drawn: red at the top, blue at the bottom" "255 0 0 ;; 0 0 255"

# n, n: the third page, turned on its side (what was its bottom left is at the top left).
WHERE=1 run 1200x900 "frame;pdfwait;key:n;key:n;frame;pdfwait;pdfpage:3" "$pdf"
set -- $(printf '%s\n' "$got" | sed -e 's/.* at //' -e 's/,/ /g')
run 1200x900 "frame;pdfwait;key:n;key:n;frame;pdfwait;frame;pdfdump;pixel:$(($1 + 20)),$(($2 + 20));pixel:$(($3 - 20)),$(($2 + 20))" "$pdf"
got=$(printf '%s' "$got" | sed -e 's/, top [0-9]*//' -e 's/, drawn [0-9+,]*//' -e 's/pixel [0-9,]*: //g')
check "the next page twice: the third, turned" "pages.pdf: ready, 3 pages, page 3, zoom 74% ;; 255 0 0 ;; 255 255 255"

# Zoomed in far: the part on show drawn again (the + after the page); 0 fits it again; G the end.
run 1200x900 "frame;pdfwait;key:=;key:=;key:=;key:=;key:=;key:=;key:=;key:=;frame;pdfwait;pdfdump;key:0;frame;pdfwait;pdfdump;key:shift+g;frame;pdfwait;pdfdump" "$pdf"
got=$(printf '%s' "$got" | sed -e 's/, top [0-9]*//g' -e 's/, drawn \([0-9]*\)+[0-9+,]*/, drawn \1+/' -e 's/, drawn [0-9,]*\([ ;]\)/\1/g' -e 's/, drawn [0-9,]*$//')
check "zoomed in far, fitted again, the end" "pages.pdf: ready, 3 pages, page 1, zoom 440%, drawn 1+ ;; pages.pdf: ready, 3 pages, page 1, zoom 74% ;; pages.pdf: ready, 3 pages, page 3, zoom 74%"

# Not a PDF.
printf 'not a PDF\n' >"$dir/bad.pdf"
run 800x600 "frame;pdfwait;frame;pdfdump" "$dir/bad.pdf"
check "a file that is not a PDF" "bad.pdf: failed (not a PDF this system can read)"

# An animation (tests/images/anim.gif: a red, a green and a blue frame,
# shown 100, 200 and 100 ms, for ever; anim.png the same as an animated
# PNG): played while its tab is on show, one frame drawn for each of its
# frames and no other; the pixels those of the frame it says it shows;
# none drawn while another tab is in front.
run 800x600 "frame;imagewait;frame;imagedump" "$pics/anim.gif"
check "an animated GIF, in its tab" "anim.gif: ready GIF 32x24 read 32x24 frame 1 of 3 playing zoom 400% middle 0.50,0.50"
run 800x600 "frame;imagewait;frame;imagedump" "$pics/anim.png"
check "an animated PNG, in its tab" "anim.png: ready PNG 32x24 read 32x24 frame 1 of 3 playing zoom 400% middle 0.50,0.50"
run 800x600 "frame;imagewait;frame;imageframes:1200" "$pics/anim.gif"
n=$(printf '%s' "$got" | sed -e 's/ frames drawn.*//')
got=$([ "$n" -ge 4 ] && [ "$n" -le 14 ] && echo "a frame for each of its frames" || echo "$n frames drawn")
check "its frames drawn as they come (about 9 in 1.2 s)" "a frame for each of its frames"
run 800x600 "frame;imagewait;frame;imageframes:150;imagedump;pixel:400,283;imageframes:260;imagedump;pixel:400,283" "$pics/anim.gif"
got=$(printf '%s' "$got" | sed -e 's/, woken [0-9]* times//g' | awk 'BEGIN { RS = " ;; " }
    / frame [0-9] of 3/ { f = $0; sub(/.* frame /, "", f); f = substr(f, 1, 1) }
    /^pixel/ { c = $0; sub(/^pixel [0-9,]*: /, "", c); sub(/\n$/, "", c); n++
               bad += !((f == 1 && c == "255 0 0") || (f == 2 && c == "0 255 0") || (f == 3 && c == "0 0 255")) }
    END { print ((n == 2 && !bad) ? "as it says" : "the frame and its pixels apart") }')
check "the frame drawn is the one it says it shows" "as it says"
run 800x600 "frame;imagewait;frame;open:$pics/pics.md;frame;imageframes:300;imageframes:800" "$pics/anim.gif"
got=$(printf '%s' "$got" | sed -e 's/^[^;]* ;; //')
check "behind another tab: no frame drawn, nothing wakes blit" "0 frames drawn, woken 0 times"

# An SVG (tests/images/shapes.svg: a red half and a blue circle, 40 by 30
# points) is text: it opens as text, and Cmd+Shift+V shows its picture
# beside it, drawn by the system at the size it is shown, and again when it
# is zoomed (it stays sharp).
run 800x600 "frame;keyof:Preview beside the file;frame;imagewait;frame;dump" "$pics/shapes.svg"
check "an SVG opens as text; Cmd+Shift+V shows its picture beside it" "H(>[shapes.svg*] | [Preview: shapes.svg*])0.50"
SVG="frame;keyof:Preview beside the file;frame;imagewait;frame;imagewait;frame"
run 1200x600 "$SVG;imagedump;key:cmd+alt+Right;key:=;frame;imagewait;frame;imagedump" "$pics/shapes.svg"
check "an SVG, drawn at the size it is shown" "shapes.svg: ready SVG 40x30 read 240x180 clear zoom 300% middle 0.50,0.50 ;; shapes.svg: ready SVG 40x30 read 300x225 clear zoom 375% middle 0.50,0.50"
WHERE=1 run 1200x600 "$SVG;imagedump" "$pics/shapes.svg"
set -- $(printf '%s\n' "$got" | sed -e 's/.* at //' -e 's/,/ /g')
run 1200x600 "$SVG;pixel:$(($1 + 30)),$(($2 + 90));pixel:$(($1 + 180)),$(($2 + 90))" "$pics/shapes.svg"
got=$(printf '%s' "$got" | sed -e 's/pixel [0-9,]*: //g')
check "its shapes where they should be" "255 0 0 ;; 0 0 255"

# In the Markdown preview: an SVG at its own size and at the width its
# <img> asks for, and an animated GIF, played while it is on show there
# and not while the preview shows other rows.
run 1400x1000 "frame;mdpreview;frame;imagewait;frame;imagewait;frame;mddump" "$pics/more.md"
got=$(printf '%s' "$got" | sed -e 's/\( ;; \)*$//' -e 's/\( ;; \)\{2,\}/ ;; /g')
check "an SVG and a GIF in the Markdown preview" "More pictures ;; ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ ;; picture: shapes.svg 5x2 ;; picture: shapes.svg 15x6 ;; picture: anim.gif 4x2"
cp "$pics/anim.gif" "$dir/anim.gif"
{ printf '# Far down\n\n'; i=0; while [ $i -lt 120 ]; do printf 'line %d\n\n' $i; i=$((i + 1)); done; printf '![frames](anim.gif)\n'; } >"$dir/far.md"
run 1400x1000 "frame;mdpreview;frame;imagewait;frame;mdwait;imageframes:300;imageframes:600;key:$doc_end;frame;mdwait;frame;imagewait;frame;imageframes:900" "$dir/far.md"
n=$(printf '%s' "$got" | sed -e 's/.* ;; //' -e 's/ frames drawn.*//')
got=$(printf '%s' "$got" | sed -e 's/ ;; [0-9]* frames drawn[^;]*$//' -e 's/^[^;]* ;; //')$([ "$n" -ge 3 ] && echo " ;; then played")
check "a GIF in the preview: played only on show" "0 frames drawn, woken 0 times ;; then played"

# A PDF's text (tests/images/text.pdf: two pages of text, a link to the web
# and one to page two, and an outline). Cmd+F finds (Enter the next one,
# Shift+Enter the one before, Esc done); a drag chooses text, two clicks a
# word, three a line, Cmd+A all of it, and Cmd+C copies it (into blit's own
# clipboard in a scripted run); a click on a link follows it (the web
# address is opened, in a scripted run only said); the outline shows in a
# box at the side (o), an entry clicked goes there.
tp="$pics/text.pdf"
run 1200x900 "frame;pdfwait;frame;pdfwait;key:cmd+f;text:fox;pdfwait;frame;pdfdump;key:Return;pdfwait;pdfdump;key:Return;pdfwait;frame;pdfdump;key:shift+Return;pdfwait;pdfdump;text:x;pdfwait;pdfdump;key:BackSpace;key:Escape;frame;pdfdump" "$tp"
got=$(printf '%s' "$got" | sed -e 's/, top [0-9]*//g' -e 's/, drawn [0-9+]*\(,[0-9][0-9+]*\)*//g' -e 's/text.pdf: ready, 2 pages, //g')
check "found with Cmd+F, Enter the next, Shift+Enter the one before, Esc done" 'page 1, zoom 95%, find "fox": *p1@31 p1@84 p2@18 ;; page 1, zoom 95%, find "fox": p1@31 *p1@84 p2@18 ;; page 2, zoom 95%, find "fox": p1@31 p1@84 *p2@18 ;; page 1, zoom 95%, find "fox": p1@31 *p1@84 p2@18 ;; page 1, zoom 95%, find "foxx": none ;; page 1, zoom 95%'
WHERE=1 run 1200x900 "frame;pdfwait;frame;pdfwait;pdftextat:1:quick;pdftextat:1:lazy dog;pdftextat:1:Go to the web;pdftextat:1:Go to page two" "$tp"
at() { printf '%s' "$got" | awk -v RS=' ;; ' -v w="1:$1 at " 'index($0, w) == 1 { s = substr($0, length(w) + 1); gsub(/,/, " ", s); print s }'; }
set -- $(at quick)
qx=$(($1 / 2)) qy=$((($2 + $4) / 4))
set -- $(at 'lazy dog')
lx=$(($3 / 2 + 2)) ly=$((($2 + $4) / 4))
set -- $(at 'Go to the web')
wx=$((($1 + $3) / 4)) wy=$((($2 + $4) / 4))
set -- $(at 'Go to page two')
px=$((($1 + $3) / 4)) py=$((($2 + $4) / 4))
run 1200x900 "frame;pdfwait;frame;pdfwait;click:$qx,$qy;drag:$lx,$ly;release;key:cmd+c;pdfwait;pdfclip;click:$((qx + 3)),$qy,2;release;key:cmd+c;pdfwait;pdfclip;click:$qx,$qy,3;release;key:cmd+c;pdfwait;pdfclip;key:cmd+a;key:cmd+c;pdfwait;pdfclip" "$tp"
check "chosen by a drag, a word, a line, all of it, and copied" "copied: quick brown fox jumps over the lazy dog. ;; copied: quick ;; copied: The quick brown fox jumps over the lazy dog. ;;  ;; copied: A page of text ;; The quick brown fox jumps over the lazy dog. ;; A second line, with the fox in it again. ;; Go to the web ;; Go to page two ;; Page two ;; One more fox, at the end."
run 1200x900 "frame;pdfwait;frame;pdfwait;pdfopened;click:$wx,$wy;release;pdfopened;click:$px,$py;release;frame;pdfwait;pdfdump" "$tp"
got=$(printf '%s' "$got" | sed -e 's/, drawn [0-9+]*\(,[0-9][0-9+]*\)*//g')
check "its links: to the web, and to page two" "opened: nothing ;; opened: https://example.org/blit ;; text.pdf: ready, 2 pages, page 2, zoom 95%, top 802"
run 1200x900 "frame;pdfwait;frame;pdfwait;pdfoutline;key:o;frame;pdfoutline;click:50,65;release;frame;pdfwait;pdfdump;click:50,80;release;frame;pdfdump;key:o;frame;pdfdump" "$tp"
got=$(printf '%s' "$got" | sed -e 's/, drawn [0-9+]*\(,[0-9][0-9+]*\)*//g')
check "its outline, in a box at the side, an entry clicked" "The first page (page 1) ;; Page two (page 2) ;;   Its last line (page 2) ;;  ;; +The first page (page 1) ;; Page two (page 2) ;;   Its last line (page 2) ;;  ;; text.pdf: ready, 2 pages, page 2, zoom 65%, top 802 ;; text.pdf: ready, 2 pages, page 2, zoom 65%, top 894 ;; text.pdf: ready, 2 pages, page 2, zoom 95%, top 894"

# Locked with a password (tests/images/locked.pdf, whose password is
# "blit"): the box asks for it; a wrong one says so, and the right one
# opens it (its blue page drawn). The password is never in what it says.
run 1200x900 "frame;pdfwait;frame;pdfdump;text:nope;pdfdump;key:Return;pdfwait;frame;pdfdump;text:blix;key:BackSpace;text:t;pdfdump;key:Return;pdfwait;frame;pdfwait;pdfdump;pixel:600,400" "$pics/locked.pdf"
got=$(printf '%s' "$got" | sed -e 's/pixel [0-9,]*: //g')
check "locked: a password asked for, a wrong one, the right one" "locked.pdf: locked, 0 typed ;; locked.pdf: locked, 4 typed ;; locked.pdf: locked, 0 typed (That password does not open it) ;; locked.pdf: locked, 4 typed (That password does not open it) ;; locked.pdf: ready, 1 pages, page 1, zoom 188%, drawn 1, top 0 ;; 0 128 255"

# On another machine: read there, by blit's agent.
T="$dir/there"
mkdir -p "$T/work"
cp "$pics/quad.png" "$pics/pages.pdf" "$T/work/"
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
run 800x600 "remotewait;frame;imagewait;frame;imagedump" "ssh://box$T/work/quad.png"
check "a picture on another machine" "quad.png: ready PNG 64x48 read 64x48 clear zoom 200% middle 0.50,0.50"
run 1200x900 "remotewait;frame;pdfwait;frame;pdfdump" "ssh://box$T/work/pages.pdf"
got=$(printf '%s' "$got" | sed -e 's/, drawn.*//')
check "a PDF on another machine" "pages.pdf: ready, 3 pages, page 1, zoom 74%"

[ -n "$print" ] || [ $failed = 1 ] || echo "images ok"
exit $failed
