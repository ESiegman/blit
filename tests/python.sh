#!/bin/sh
# The project's Python (src/apppython.cpp): one for each project folder and
# subproject (a folder in it with its own pyproject.toml, .venv, venv or
# uv.lock), found on a worker and kept; looked at again when a venv comes or
# goes (the watcher, or blit coming to the front) and when asked; pinned by
# blit.opt.python.interpreter, or chosen from the status bar for the project
# (kept in its session); named in the status bar; taken by the Python
# console. The Pythons here are stand-ins (a
# small script answering what blit asks a Python: its version, its path,
# whether it has debugpy), so that what is shown is the same everywhere;
# anything else they are asked to run, python3 runs.
#
#   tests/python.sh path/to/blit            run the cases
#   tests/python.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/python.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
command -v python3 >/dev/null 2>&1 || { echo "python skipped: no python3"; exit 0; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" XDG_DATA_HOME="$dir/data"
real=$(python3 -c 'import sys; print(sys.version.split()[0])')  # (python3's own, where it is the one: the PATH's)

failed=0
check() {
    # (Whether a grammar is installed is this machine's; python3's version too.)
    got=$(printf '%s' "$got" | sed -E -e 's/ \(no colours\)//g' -e "s/Python ${real%.*}([ |])/Python V\1/g" -e "s/ $real( |$)/ V\1/g")
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
join() { awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }'; }
# run SCRIPT [FILE]: what the run printed, its lines joined by " ;; ".
run() {
    got=$("$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null | sed -e "s#$dir#@#g" | join)
}
# A venv's stand-in Python: venv FOLDER VERSION [SECONDS]. Asked what it is,
# it says VERSION (after SECONDS, if given), and writes its path to
# $dir/asked; asked to run anything else, it writes "ran", its path and
# what it was given to $dir/ran, and python3 runs it.
cat >"$dir/venv.sh" <<EOF
#!/bin/sh
mkdir -p "\$1/bin" && printf 'home = /usr/bin\n' >"\$1/pyvenv.cfg"
cat >"\$1/bin/python" <<END
#!/bin/sh
if [ "\\\$1" = -c ]; then
    echo "\\\$0" >>"$dir/asked"
    [ -z "\$3" ] || sleep \$3
    printf '%s\n%s\n0\n' "\$2" "\\\$0"
    exit 0
fi
echo "ran \\\$0 \\\$*" >>"$dir/ran"
exec python3 "\\\$@"
END
chmod +x "\$1/bin/python"
EOF
venv() { sh "$dir/venv.sh" "$@"; }

# A project: its .venv; a second folder of it (the same Python, asked once);
# a subproject with a pyproject.toml and no venv (the PATH's), and one with
# a venv of its own.
P="$dir/p"
mkdir -p "$P/src" "$P/tests" "$P/sub" "$P/sub2"
venv "$P/.venv" 3.99.1
venv "$P/sub2/.venv" 3.98.2
printf '[project]\nname = "sub"\n' >"$P/sub/pyproject.toml"
for f in src/a.py tests/t.py sub/b.py sub2/c.py; do printf 'x = 1\n' >"$P/$f"; done
rm -f "$dir/asked"
run "workdir:$P;frame;pywait;frame;statusdump;open:$P/tests/t.py;frame;pywait;frame;statusdump;open:$P/sub2/c.py;frame;pywait;frame;statusdump;open:$P/sub/b.py;frame;pywait;frame;statusdump;pydump" "$P/src/a.py"
check "each folder its project's Python, a subproject's its own: in the status bar" \
    'src/a.py | Python 3.99 (.venv) Ln 1, Col 1 Spaces: 4 LF Python ;; tests/t.py | Python 3.99 (.venv) Ln 1, Col 1 Spaces: 4 LF Python ;; sub2/c.py | Python 3.98 (.venv) Ln 1, Col 1 Spaces: 4 LF Python ;; sub/b.py | Python V Ln 1, Col 1 Spaces: 4 LF Python ;; @/p/src = @/p (.venv) @/p/.venv/bin/python 3.99.1 | @/p/sub = @/p/sub (PATH) python3 V | @/p/sub2 = @/p/sub2 (.venv) @/p/sub2/.venv/bin/python 3.98.2 | @/p/tests = @/p (.venv) @/p/.venv/bin/python 3.99.1'
got=$(sed -e "s#$dir#@#g" "$dir/asked" | sort | join)
check "the project's Python asked once for its two folders" '@/p/.venv/bin/python ;; @/p/sub2/.venv/bin/python'

# Nothing waits for it: a Python slow to answer, the file shown meanwhile
# without it, and with it once it has answered.
S="$dir/slow"
mkdir -p "$S"
venv "$S/.venv" 3.97.0 2
printf 'x = 1\n' >"$S/a.py"
run "workdir:$S;clock;frame;key:Down;frame;within:500;statusdump;pywait;frame;statusdump" "$S/a.py"
check "a Python slow to answer: nothing waits for it" 'within 500 ms ;; a.py | Ln 2, Col 1 Spaces: 4 LF Python ;; a.py | Python 3.97 (.venv) Ln 2, Col 1 Spaces: 4 LF Python'

# Looked at again: a venv made in the folder, and taken away, as the
# watcher hears; one made in a folder that is not watched, when blit comes
# to the front; and the interpreter asked again only when asked for.
W="$dir/w"
mkdir -p "$W"
printf 'x = 1\n' >"$W/w.py"
run "workdir:$W;key:cmd+B;panel;frame;pywait;frame;statusdump;watchheld:sh $dir/venv.sh $W/.venv 3.96.0;watchwait;pywait;frame;statusdump;watchheld:rm -rf $W/.venv;watchwait;pywait;frame;statusdump" "$W/w.py"
check "a venv made, then taken away: heard, and the status bar follows" 'w.py | Python V Ln 1, Col 1 Spaces: 4 LF Python ;; w.py | Python 3.96 (.venv) Ln 1, Col 1 Spaces: 4 LF Python ;; w.py | Python V Ln 1, Col 1 Spaces: 4 LF Python'
F="$dir/f"
mkdir -p "$F"
printf '[project]\nname = "f"\n' >"$F/pyproject.toml"
printf 'x = 1\n' >"$F/f.py"
run "frame;pywait;frame;statusdump;lua:os.execute('sh $dir/venv.sh $F/.venv 3.95.0');focus;pywait;frame;statusdump;lua:os.execute('sh $dir/venv.sh $F/.venv 3.94.0');focus;pywait;frame;statusdump;command:Find the project's Python again;pywait;frame;statusdump" "$F/f.py"
check "made where nothing watches: found when blit comes to the front; asked again when asked to be" \
    'f.py | Python V Ln 1, Col 1 Spaces: 4 LF Python ;; f.py | Python 3.95 (.venv) Ln 1, Col 1 Spaces: 4 LF Python ;; f.py | Python 3.95 (.venv) Ln 1, Col 1 Spaces: 4 LF Python ;; f.py | Python 3.94 (.venv) Ln 1, Col 1 Spaces: 4 LF Python'

# Pinned by the settings: a path (over the project's .venv), one in the
# project ("zqpin/bin/python"), and a command; changed while blit runs.
venv "$dir/pinned" 3.93.0
venv "$P/zqpin" 3.92.0
printf 'blit.opt.python.interpreter = "%s"\n' "$dir/pinned/bin/python" | cat "$dir/init.lua" - >"$dir/pin.lua"
got=$(BLIT_INIT="$dir/pin.lua" "$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "workdir:$P;frame;pywait;frame;statusdump;lua:blit.opt.python.interpreter = 'zqpin/bin/python';pywait;frame;statusdump;lua:blit.opt.python.interpreter = 'python3';pywait;frame;statusdump;lua:blit.opt.python.interpreter = '';pywait;frame;statusdump" "$P/src/a.py" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | join)
check "pinned: a path, one in the project, a command, and none again" \
    'src/a.py | Python 3.93 (pinned) Ln 1, Col 1 Spaces: 4 LF Python ;; src/a.py | Python 3.92 (pinned) Ln 1, Col 1 Spaces: 4 LF Python ;; src/a.py | Python V (pinned) Ln 1, Col 1 Spaces: 4 LF Python ;; src/a.py | Python 3.99 (.venv) Ln 1, Col 1 Spaces: 4 LF Python'

# Chosen from the status bar (a click on it lists the Pythons there are):
# kept in the project's session, and back when the project is opened again.
venv "$P/zqenv" 3.91.0
grep -v restore "$dir/init.lua" >"$dir/kept.lua"
got=$(BLIT_INIT="$dir/kept.lua" "$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "folder:$P;open:$P/src/a.py;frame;pywait;frame;statusclick:Python 3.99;pywait;finddump;text:zqenv;key:Return;pywait;frame;statusdump;quit" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | join)
# (Of the list, the venvs in the project, and the row the one chosen is on;
# the others there are are this machine's.)
got=$(printf '%s' "$got" | sed -e 's/ ;; pick [^;]* : / ;; /' | awk 'BEGIN { RS = " ;; "; ORS = "" }
    NR == 2 { n = split($0, w, " Python "); $0 = ""; for (i = 1; i <= n; i++) if (w[i] ~ /@\/p\/|Automatic/) $0 = $0 (i > 1 ? " Python " : "") w[i] }
    { printf "%s%s", (NR > 1 ? " ;; " : ""), $0 }')
check "the list: the project's venvs among them; one chosen" \
    'blit: Looking for Pythons… ;; >Automatic Python 3.91.0 · zqenv · @/p/zqenv/bin/python Python 3.92.0 · zqpin · @/p/zqpin/bin/python Python 3.99.1 · .venv · @/p/.venv/bin/python ;; src/a.py | Python 3.91 (chosen) Ln 1, Col 1 Spaces: 4 LF Python'
got=$(grep -h '^python	' "$dir"/sessions/* 2>/dev/null | sed -e "s#$dir#@#g" | join)
check "kept in the project's session" 'python	@/p	@/p/zqenv/bin/python'
got=$(BLIT_INIT="$dir/kept.lua" "$blit" --offscreen 1400x800 --shot "$dir/shot.bmp" --script "frame;pywait;frame;statusdump;statusclick:Python 3.91;pywait;text:Automatic;key:Return;pywait;frame;statusdump;quit" "$P" 2>&1 >/dev/null | grep -v '^blit: ' | sed -e "s#$dir#@#g" | join)
check "back with the project; Automatic gives it back to the project" 'src/a.py | Python 3.91 (chosen) Ln 1, Col 1 Spaces: 4 LF Python ;; src/a.py | Python 3.99 (.venv) Ln 1, Col 1 Spaces: 4 LF Python'
got=$(grep -h '^python	' "$dir"/sessions/* 2>/dev/null | join)
check "and none kept" ''

# The Python console: the subproject's Python of the file in front.
rm -f "$dir/ran"
run "workdir:$P;open:$P/sub2/c.py;command:Open the Python console;termwait:>>>" "$P/src/a.py"
got=$(sed -e "s#$dir#@#g" "$dir/ran" | join)
check "the console: the subproject's Python" 'ran @/p/sub2/.venv/bin/python '

[ -n "$print" ] || [ $failed = 1 ] || echo "python ok"
exit $failed
