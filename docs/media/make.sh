#!/bin/sh
# The README's screenshots and animations (docs/media/*.png and *.gif), made
# by a Debug build's offscreen harness on the demo project beside this file:
#
#   docs/media/make.sh build-debug/blit [NAME...]
#
# With names, only those are made again. Needs ffmpeg for the animations,
# clangd for the language-server shots, lldb-dap (Xcode) for the debugger
# and xctrace for the profile. Nothing outside a scratch folder is touched:
# HOME, the settings, the caches and the demo's git repository all live in it.
set -eu
blit=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
here=$(cd "$(dirname "$0")" && pwd)
work=$(cd "$(mktemp -d)" && pwd -P)  # (resolved: macOS's temporary folder is reached through /private)
trap 'rm -rf "$work"' EXIT
want() { [ -z "$WANT" ] && return 0; for n in $WANT; do [ "$n" = "$1" ] && return 0; done; return 1; }
WANT="$*"

export HOME="$work/home" BLIT_APPEARANCE=dark BLIT_RECENT_FILE="$work/recent" BLIT_CACHE_DIR="$work/cache" BLIT_HISTORY_DIR="$work/history"
unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME
mkdir -p "$HOME/.config/blit" "$HOME/.local/share"
# A terminal's shell with a plain prompt (no user or machine name in a picture).
printf "PROMPT='%%F{blue}%%1~%%f %%# '\n" >"$HOME/.zshrc"
# The grammars this machine has, for the colours.
[ -d "/Users/$(id -un)/.local/share/blit" ] && ln -s "/Users/$(id -un)/.local/share/blit" "$HOME/.local/share/blit"
for d in /opt/homebrew/opt/llvm/bin /Library/Developer/CommandLineTools/usr/bin; do [ -x "$d/clangd" ] && PATH="$d:$PATH" && break; done
export PATH
# The settings: Vim's modes, and the language servers only where a shot
# is about them (lsp.lua). (A shell keeps a variable set before a function
# call after it, so BLIT_INIT is set and taken away around that shot.)
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = true\n' >"$HOME/.config/blit/init.lua"
printf 'blit.opt.lsp.inlay_hints = true\nblit.opt.editor.modal = true\n' >"$HOME/.config/blit/lsp.lua"
unset BLIT_INIT

# The demo project, as a git repository with a commit and changes since
# (in the scratch home, so that it shows as ~/orbits).
demo="$HOME/orbits"
cp -R "$here/demo" "$demo"
(
    cd "$demo"
    git init -q -b main
    git add -A
    git -c user.name=blit -c user.email=blit@example.invalid commit -qm "Orbits: the sun, the earth and the moon"
    sed -i '' 's/const double r = std::max(d.length(), 1e-3);/const double r = std::max(d.length(), 1e-6);  \/\/ closer before it is cut off/' src/body.cpp
    sed -i '' 's/        {"moon", {1.05, 0}, {0, 36.1}, 0.01},/        {"moon", {1.05, 0}, {0, 36.1}, 0.01},\n        {"mars", {1.52, 0}, {0, 25.6}, 0.1},/' src/main.cpp
)

W=2560 H=1600
# shot NAME SCRIPT [FILE]: one frame of the window after SCRIPT, as NAME.png.
shot() {
    name=$1 script=$2
    shift 2
    "$blit" --offscreen ${W}x$H --shot "$work/$name.bmp" --script "$script" "$@" >"$work/$name.out" 2>"$work/$name.log" || true
    if [ -f "$work/$name.bmp" ]; then
        sips -s format png "$work/$name.bmp" --out "$here/$name.png" >/dev/null
        echo "$name.png"
    else
        echo "$name: no frame (see $work/$name.log)" >&2
        cat "$work/$name.log" >&2
    fi
}
# anim NAME FPS STEP...: one frame after each step (each step's script is
# the steps before it and itself), made into NAME.gif, from a smaller
# window than the stills' (AW x AH) at three quarters, so that its text
# reads at the width the README shows it.
AW=1600 AH=1000
anim() {
    name=$1 fps=$2
    shift 2
    frames="$work/$name.frames"
    mkdir -p "$frames"
    script="" n=0
    for s in "$@"; do
        script="${script:+$script;}$s"
        [ -n "${ANIM_RESET:-}" ] && eval "$ANIM_RESET"  # (what a step changes on disk, put back before the next replays them all)
        "$blit" --offscreen ${AW}x$AH --shot "$frames/f.bmp" --script "$script" $ANIM_FILE >/dev/null 2>&1 || true
        sips -s format png "$frames/f.bmp" --out "$(printf '%s/%03d.png' "$frames" "$n")" >/dev/null
        n=$((n + 1))
    done
    [ -n "${ANIM_RESET:-}" ] && eval "$ANIM_RESET"
    ffmpeg -loglevel error -y -framerate "$fps" -i "$frames/%03d.png" \
        -vf "scale=$((AW * 3 / 4)):-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=none" \
        "$here/$name.gif"
    echo "$name.gif"
}

cd "$demo"

# The editor on the demo project: the side panel's tree, a file with its
# colours, and the README's preview beside it.
want hero && shot hero "folder:$demo;key:cmd+b;panel;open:$demo/README.md;frame;mdpreview;mdwait;imagewait;frame;mdwait;open:$demo/src/body.cpp;frame;frame"

# Language servers: clangd's completions, a problem underlined with its
# message, parameter names as inlay hints, and colours from the server.
if want lsp && command -v clangd >/dev/null; then
    export BLIT_INIT="$HOME/.config/blit/lsp.lua"
    cp src/main.cpp "$work/main.cpp"
    sed -i '' 's|    std::printf("energy drift: %.2e\\n", (after - before) / before);|    const double drift = (after - before) / befor;\n    std::printf("energy drift: %.2e\\n", drift);|' src/main.cpp
    shot lsp "open:$demo/src/main.cpp;ready;lspsettle;wait:1500;frame;wait:800;text::22;key:Return;text:o;frame;text:bodies.front().;wait:1500;frame;key:ctrl+space;wait:1500;frame;wait:300;frame"
    cp "$work/main.cpp" src/main.cpp
    unset BLIT_INIT
fi


# Go to file, narrowing as letters are typed.
ANIM_FILE="$demo/src/main.cpp"
want find && anim find 3 "folder:$demo;frame" "key:cmd+p;findwait" "text:b;findwait" "text:o;findwait" "text:d;findwait" "text:y;findwait" "frame" "key:Return;frame" "frame"

# Search in files.
want search && shot search "folder:$demo;open:$demo/src/body.cpp;frame;key:cmd+shift+f;text:bodies position;findwait;frame;frame"

# Splits, and a terminal along the bottom.
want splits && shot splits "folder:$demo;open:$demo/src/main.cpp;frame;key:cmd+\\;text:body.hpp;findwait;key:Return;frame;key:cmd+\\;text:body.cpp;findwait;key:Return;frame;command:Show or hide the terminal;termwait:%;text:git --no-pager log --stat -1;key:Return;termwait:insertions;frame;frame"

# Git: the Git tab, and a diff in two columns.
want git && shot git "folder:$demo;open:$demo/src/body.cpp;key:cmd+b;panel;command:Show Git;gitwait;git:diff:src/body.cpp;gitwait;git:columns;gitwait;frame;frame"

# The debugger, stopped at a breakpoint: the stack, the variables, a watch.
if want debug; then
    g++-16 -g -O0 -std=c++23 -Isrc src/main.cpp src/body.cpp -o "$work/orbits-debug" 2>/dev/null &&
        shot debug "debug:program=$work/orbits-debug;debug:args=50;open:$demo/src/body.cpp;break:17;key:F5;debugwait:stopped;debugpanel;frame;frame"
fi

# A profile of the program: its flame graph.
if want profile; then
    g++-16 -g -O2 -std=c++23 -Isrc src/main.cpp src/body.cpp -o "$work/orbits" &&
        shot profile "prof:program=$work/orbits;prof:args=40000000;profrecord;profwait;frame;frame"
fi

# The Markdown preview: a sequence diagram, a table, coloured code, a pie.
want markdown && shot markdown "open:$demo/notes.md;frame;mdpreview;mdwait;frame;mdwait;frame"

# A PDF, with what Cmd+F found on its pages.
if want pdf; then
    clang -fobjc-arc "$here/tools/pdf.m" -framework AppKit -o "$work/pdf" 2>/dev/null &&
        "$work/pdf" "$demo/src/body.cpp" "$demo/body.pdf" &&
        shot pdf "open:$demo/body.pdf;pdfwait;frame;key:cmd+f;text:bodies;pdfwait;frame;frame"
fi

# The start screen, with what was opened lately.
if want start; then
    printf 'd\t%s\nf\t%s/src/main.cpp\nf\t%s/README.md\nf\t%s/notes.md\n' "$demo" "$demo" "$demo" "$demo" >"$BLIT_RECENT_FILE"
    shot start "frame;frame"
fi

# Choosing a theme: each put on as the list comes to it.
ANIM_FILE="$demo/src/body.cpp"
want theme && anim theme 2 "frame" "command:Choose a theme;frame" "key:Down;frame" "key:Down;frame" "key:Down;frame" "key:Down;frame" "key:Down;frame" "key:Up;frame"

# The command search: every command, with its keys.
want commands && shot commands "open:$demo/src/main.cpp;frame;key:cmd+shift+p;text:split;frame;frame"

# A new project, from a form.
want newproject && shot newproject "newproject;project:name=gravity;project:where=$work/projects;frame;frame"

# Peek: the definition of step() in a box under the line that calls it.
if want peek && command -v clangd >/dev/null; then
    export BLIT_INIT="$HOME/.config/blit/lsp.lua"
    shot peek "open:$demo/src/main.cpp;ready;lspsettle;wait:1500;text:/step(;key:Return;command:Peek the definition;wait:1500;frame;wait:500;frame"
    unset BLIT_INIT
fi

# The calls to step(), in the side panel.
if want calls && command -v clangd >/dev/null; then
    export BLIT_INIT="$HOME/.config/blit/lsp.lua"
    shot calls "folder:$demo;open:$demo/src/main.cpp;ready;lspsettle;wait:1500;open:$demo/src/body.cpp;ready;lspsettle;wait:1500;text:/step(;key:Return;command:Show the calls to this function;wait:2000;frame;key:Right;wait:1500;frame;frame"
    unset BLIT_INIT
fi

# Local history: the file as it was at each save, the one chosen against now.
if want history; then
    cp src/body.cpp "$work/body.cpp"
    # (Saved in one run, listed in the next, as the history is kept when it goes.)
    "$blit" --offscreen ${W}x$H --shot "$work/saves.bmp" --script "open:$demo/src/body.cpp;frame;text:G;text:o;key:Return;text:// The step's cost grows as the square of the bodies.;key:Escape;text::w;key:Return;wait:1100;text::12;key:Return;text:A  // never zero;key:Escape;text::w;key:Return;frame" >/dev/null 2>&1
    shot history "open:$demo/src/body.cpp;frame;command:Show the file's local history;frame;key:Down;frame;frame"
    cp "$work/body.cpp" src/body.cpp
fi

# The assembly of the file, beside it, each line's instructions lit with it.
want asm && shot asm "open:$demo/src/body.cpp;frame;text:/pull\\[i\\] = ;key:Return;command:Show the assembly beside the file;asmdump;text:jk;frame;frame"

# A notebook: its cells, their outputs.
want notebook && shot notebook "open:$demo/orbits.ipynb;frame;wait:300;frame"


# Vim: a word changed, then the change repeated at the next ones with n and .
ANIM_FILE="$demo/src/main.cpp"
want vimdot && anim vimdot 2 "frame" "text:/gravity;key:Return;frame" "text:ciw;frame" "text:g;frame" "key:Escape;frame" \
    "text:n;frame" "text:.;frame" "text:n;frame" "text:.;frame" "text:n;frame" "text:.;frame" "frame"

# Several carets: the next match added to them, each taking the change.
ANIM_FILE="$demo/src/body.cpp"
want carets && anim carets 2 "frame" "text:/bodies;key:Return;frame" "command:Add the next match to the carets;frame" \
    "command:Add the next match to the carets;frame" "command:Add the next match to the carets;frame" \
    "command:Add the next match to the carets;frame" "text:c;frame" "text:bs;frame" "key:Escape;frame" "frame"

# The settings screen: a switch flipped, a theme picked, each written into init.lua.
ANIM_FILE="$demo/src/body.cpp"
if want settings; then
    cp "$HOME/.config/blit/init.lua" "$work/init.lua"
    ANIM_RESET='cp "$work/init.lua" "$HOME/.config/blit/init.lua"'
    anim settings 1 "frame" "key:cmd+,;frame" "text:/editor.relative_numbers;key:Return;text::noh;key:Return;frame" "key:Return;frame" \
        "text:/theme.dark;key:Return;text::noh;key:Return;frame" "key:Return;frame" "key:Down;frame" "key:Down;frame" "key:Return;frame" "frame"
    ANIM_RESET=
fi

# Distraction-free, then presentation mode.
ANIM_FILE=
want focus && anim focus 1 "folder:$demo;key:cmd+b;panel;open:$demo/src/body.cpp;frame" "command:Toggle distraction-free mode;frame" \
    "command:Toggle distraction-free mode;command:Toggle presentation mode;frame" "command:Toggle presentation mode;frame"

# The tree keeps up with the disk: a file made and one removed by another
# program, then a file dragged into a folder.
if want tree; then
    ANIM_RESET='rm -rf "$demo/plot.py" "$demo/data"; git -C "$demo" checkout -q -- logo.png; [ -f "$demo/src/notes.md" ] && mv "$demo/src/notes.md" "$demo/notes.md"; true'
    anim tree 2 "folder:$demo;key:cmd+b;panel;open:$demo/README.md;frame" "lua:os.execute('echo import math > $demo/plot.py');watchwait;frame" \
        "lua:os.execute('mkdir $demo/data');watchwait;frame" "lua:os.execute('rm $demo/logo.png');watchwait;frame" \
        "click:60,104;drag:70,96;frame" "drag:65,75;frame" "drag:60,57;frame" "release;panel;frame" "click:60,57;release;panel;frame" "frame"
    ANIM_RESET=
fi

# A tab dragged to the right edge: a split of its own.
ANIM_FILE=
want splitdrag && anim splitdrag 3 "open:$demo/src/main.cpp;open:$demo/src/body.cpp;frame" "click:193,12;drag:210,40;frame" \
    "drag:450,200;frame" "drag:650,240;frame" "drag:785,250;frame" "release;frame" "frame"
