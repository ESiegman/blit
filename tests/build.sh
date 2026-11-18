#!/bin/sh
# The Build tab and the start screen's New project tab: what each kind of
# folder is found to build with, init.lua's own systems (blit.build.add),
# running a task in the output terminal, and new projects made from the
# form (and init.lua's own kinds, blit.project.add). vcpkg is a stand-in
# folder here: its catalog and a port's usage notes, nothing installed.
#
#   tests/build.sh path/to/blit            run the cases
#   tests/build.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/build.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"  # (new projects go in ~/Dev by default)
mkdir -p "$HOME/Dev"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"

# Folders of each kind.
mkdir -p "$dir/cm" "$dir/pre" "$dir/mk" "$dir/ms" "$dir/uv/tests" "$dir/pip/.venv" "$dir/rs" "$dir/none"
printf 'project(x)\n' >"$dir/cm/CMakeLists.txt"
cp "$dir/cm/CMakeLists.txt" "$dir/pre/CMakeLists.txt"
cat >"$dir/pre/CMakePresets.json" <<'EOF'
{"version": 6,
 "configurePresets": [{"name": "base", "hidden": true, "binaryDir": "${sourceDir}/out/${presetName}"}, {"name": "dev", "inherits": "base"}],
 "buildPresets": [{"name": "dev", "configurePreset": "dev"}]}
EOF
printf 'CC := cc\nall:\n\techo built-it\nx.o: x.c\ntest install:\n\ttrue\nclean:\n\trm -f x\n%%.o: %%.c\nfail:\n\texit 2\n' >"$dir/mk/Makefile"
printf "project('x', 'c')\n" >"$dir/ms/meson.build"
printf 'print(1)\n' >"$dir/uv/main.py"
: >"$dir/uv/uv.lock"
printf '[project]\nname = "p"\n' >"$dir/pip/pyproject.toml"
printf '[package]\nname = "rs"\n' >"$dir/rs/Cargo.toml"

# A stand-in vcpkg: its catalog, and fmt's usage notes.
V="$dir/vcpkg"
mkdir -p "$V/scripts/buildsystems" "$V/ports/fmt" "$V/ports/zlib" "$V/versions"
: >"$V/scripts/buildsystems/vcpkg.cmake"
printf '{"default": {"fmt": {"baseline": "11.0.2"}, "zlib": {"baseline": "1.3.1"}}}\n' >"$V/versions/baseline.json"
printf '{"name": "fmt", "description": "Formatting"}\n' >"$V/ports/fmt/vcpkg.json"
printf '{"name": "zlib", "description": "Compression"}\n' >"$V/ports/zlib/vcpkg.json"
printf 'fmt provides CMake targets:\n\n    find_package(fmt CONFIG REQUIRED)\n    target_link_libraries(main PRIVATE fmt::fmt)\n\n    # Or\n    find_package(fmt CONFIG REQUIRED)\n    target_link_libraries(main PRIVATE fmt::fmt-header-only)\n' >"$V/ports/fmt/usage"
export VCPKG_ROOT="$V"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FOLDER SCRIPT: the last line the run printed. INIT, if set, is the
# init.lua for this run only.
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$1;frame;$2" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -1)
    INIT=
}

# What each folder builds with.
run "$dir/cm" "builddump"
check "CMakeLists.txt, never configured" 'CMake: >Build Configure Test'
run "$dir/pre" "builddump"
check "presets: the ones shown, a build preset's folder by its preset" 'CMake: Configure (dev) >Build (dev)'
run "$dir/mk" "builddump"
check "a Makefile: make, and its targets (not files, patterns or variables)" 'Make: >make make all make test make install make clean make fail'
run "$dir/ms" "builddump"
check "meson.build, not set up yet" 'Meson: >Build Set up'
run "$dir/uv" "builddump"
check "uv: run, test, sync" 'Python: >Run Test Sync'
run "$dir/pip" "builddump"
check "pip: its .venv's python" 'Python: >Install (editable)'
run "$dir/rs" "builddump"
check "other languages are init.lua's to add" 'nothing'

# init.lua's own.
cat >"$dir/cargo.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Cargo", {detect = "Cargo.toml", tasks = {{name = "Build", cmd = "cargo build", default = true}, "cargo test"}})
EOF
INIT="$dir/cargo.lua"
run "$dir/rs" "builddump"
check "blit.build.add: files and a list" 'Cargo: >Build cargo test'
cat >"$dir/fn.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Mine", {first = true,
  detect = function(root, files) for _, f in ipairs(files) do if f.name == "Makefile" then return true end end end,
  tasks = function(root, files) return {{name = "Hi", cmd = "echo hi from " .. #files}} end})
blit.build.remove("Make")
EOF
INIT="$dir/fn.lua"
run "$dir/mk" "lua:local t = blit.build.tasks() print(t[1].system .. ' ' .. t[1].cmd .. ' ' .. #t .. ' ' .. table.concat(blit.build.systems(), ','))"
check "functions, first, and a built-in taken away" 'blit: Mine echo hi from 1 1 Mine,CMake,Meson,Autotools,Ninja,Python'

# Running: in the output terminal, how it ended.
run "$dir/mk" "buildrun;buildwait;builddump"
check "the Run button's task, to its end" 'Make: >make make all make test make install make clean make fail (last: 0)'
run "$dir/mk" "buildrun:make fail;buildwait;builddump"
check "a task that fails: its status, and it is the Run button's now" 'Make: make make all make test make install make clean >make fail (last: 2)'
run "$dir/mk" "lua:print(blit.build.run('make clean'), blit.build.run('no such'));buildwait;builddump"
check "blit.build.run" 'Make: make make all make test make install >make clean make fail (last: 0)'

# Problems a build reports: a fake one printing what compilers, CMake,
# Ninja, make and Python print (GCC's colours, Ninja's progress written over
# itself, a header's warning twice), its files there to go to.
F="$dir/fb"
mkdir -p "$F/src" "$F/lib" "$F/sub" "$F/cmake"
printf 'int a;\nint b;\nint c = x;\nint d;\nint e;\nint f;\nint g = y;\n' >"$F/src/a.c"
printf '#pragma once\nint unused;\n' >"$F/src/b.h"
printf '\n\n\n  bad();\n' >"$F/lib/c.c"
printf 'one\ntwo\n' >"$F/sub/d.c"
printf 'cmake_minimum_required(VERSION 3.20)\nmessage(WARNING "look here")\n' >"$F/cmake/x.cmake"
printf 'import sys\nraise ValueError("boom")\n' >"$F/tool.py"
cat >"$F/errs.sh" <<'EOF'
sleep 0.2  # (the output split laid out first, as a window does at once: a terminal is 80 columns until then)
e=$(printf '\033')
printf '%s[01m%s[Ksrc/a.c:3:9:%s[m%s[K %s[01;31m%s[Kerror: %s[m%s[K'"'"'x'"'"' undeclared\n' $e $e $e $e $e $e $e $e
printf '    3 | int c = x;\n      |         ^\n'
printf 'src/b.h:2:5: warning: unused [-Wunused]\nsrc/b.h:2:5: warning: unused [-Wunused]\n'
printf 'src/a.c:3:9: note: each undeclared identifier is reported only once\ng++: error: no input files\n'
printf '\r[1/2] Building C object a.o\033[K\r[2/2] Linking C executable p\033[K\r\n'
printf 'ninja: Entering directory `lib'"'"'\nFAILED: c.o\nc.c:4:3: error: bad\n'
printf 'make -C sub\nd.c:2:1: warning: in a folder make went into\n'
printf 'CMake Warning at cmake/x.cmake:2 (message):\n  look here\n\n\n'
printf 'Traceback (most recent call last):\n  File "%s/tool.py", line 2, in <module>\n    raise ValueError("boom")\nValueError: boom\n' "$(pwd -P)"
printf 'src/a.c:7:9: error: second\n'
exit 2
EOF
printf 'errors:\n\tsh errs.sh\nok:\n\techo fine\n' >"$F/Makefile"
run "$F" "buildrun:make errors;buildwait;problemsdump"
check "a build's problems: a file's together, in the order the files came" 'button: ▶ make errors · 4 errors | Build 7 | file src/a.c | >E 3:9 '"'"'x'"'"' undeclared | E 7:9 second | file src/b.h | W 2:5 unused [-Wunused] | file lib/c.c | E 4:3 bad | file sub/d.c | W 2:1 in a folder make went into | file cmake/x.cmake | W 2 look here | file tool.py | E 2 ValueError: boom'
run "$F" "buildrun:make errors;buildwait;frame;statusdump"
check "counted in the status bar" 'NORMAL untitled | × 4 ▲ 3 Ln 1, Col 1 LF Plain text'
run "$F" "buildrun:make errors;buildwait;key:F8;key:F8;key:F8;frame;statusdump"
check "F8: the next problem, across files" 'NORMAL src/b.h | × 4 ▲ 3 Ln 2, Col 5 Tabs LF C++'
run "$F" "buildrun:make errors;buildwait;key:F8;key:F8;key:F8;problemsdump"
got=${got#*| Build 7 | }
check "and the Problems tab picks it" 'file src/a.c | E 3:9 '"'"'x'"'"' undeclared | E 7:9 second | file src/b.h | >W 2:5 unused [-Wunused] | file lib/c.c | E 4:3 bad | file sub/d.c | W 2:1 in a folder make went into | file cmake/x.cmake | W 2 look here | file tool.py | E 2 ValueError: boom'
run "$F" "buildrun:make errors;buildwait;key:shift+F8;frame;statusdump"
check "Shift+F8 from the start: the last" 'NORMAL tool.py | × 4 ▲ 3 Ln 2, Col 1 Tabs LF Python'
run "$F" "buildrun:make errors;frame;buildwait;frame;termclick:c.c:4:3;frame;statusdump"
check "Cmd+click on a path:line in the output: from the folder Ninja went into" 'NORMAL lib/c.c | × 4 ▲ 3 Ln 4, Col 3 Spaces: 2 LF C'
run "$F" "buildrun:make errors;frame;buildwait;frame;termclick:tool.py\", line 2;frame;statusdump"
check "and on Python's File \"path\", line N (a long line, wrapped)" 'NORMAL tool.py | × 4 ▲ 3 Ln 2, Col 1 Tabs LF Python'
run "$F" "buildrun:make errors;frame;buildwait;frame;termclick:Linking;frame;statusdump"
check "Cmd+click on what is not a place: nothing opens (the click is the terminal's)" 'make errors | × 4 ▲ 3'
run "$F" "buildrun:make errors;buildwait;buildrun:make ok;buildwait;problemsdump"
check "the next run starts with none" 'button: ▶ make ok | file 0'
# CMake's build type and target, chosen in the Build tab (Enter on each):
# Release, then the target app of the folder's own (CMake's help lists
# them); Build configures with the type and builds that target alone.
if command -v cmake >/dev/null 2>&1; then
    T="$dir/types"
    mkdir -p "$T"
    printf 'cmake_minimum_required(VERSION 3.20)\nproject(two C)\nadd_executable(app app.c)\nadd_executable(tool tool.c)\n' >"$T/CMakeLists.txt"
    printf 'int main(void) { return 0; }\n' >"$T/app.c"
    cp "$T/app.c" "$T/tool.c"
    (cd "$T" && cmake -S . -B build >/dev/null 2>&1)
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;key:cmd+shift+u;builddump;frame;key:Up;key:Up;key:Return;key:Return;key:Down;key:Return;finddump;key:Down;key:Return;frame;buildrun:Build;buildwait;builddump" "$T/CMakeLists.txt" 2>&1 >/dev/null | tail -2 | tr '\n' '/')
    got="$got $(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$T/build/CMakeCache.txt") $(ls "$T/build" | grep -E '^(app|tool)$' | tr '\n' ' ')"
    check "CMake: the build type and a target chosen, and built so" 'pick [] 3 : >all app tool/CMake: >Build Test Clean Configure (last: 0)/ Release app '
fi

# The sanitizers' reports, as a program run prints them: AddressSanitizer
# (its first frame the system's, the next the program's own), UBSan's
# runtime error, ThreadSanitizer's race, LeakSanitizer's leaks (each a
# problem); and a program really built with AddressSanitizer, run.
S="$dir/san"
mkdir -p "$S"
printf '#include <stdlib.h>\nint main(void)\n{\n    int* p = malloc(4 * sizeof(int));\n    return p[4];\n}\n' >"$S/a.c"
printf 'int f(void);\nint g(void);\nint h(void);\n' >"$S/t.c"
cat >"$S/reports.sh" <<EOF
sleep 0.2
printf '=================================================================\n==4242==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000014 at pc 0x1 bp 0x2 sp 0x3\n'
printf 'READ of size 4 at 0x602000000014 thread T0\n    #0 0x7fff in memcpy (/usr/lib/libc.so.6+0x10)\n    #1 0x5555 in main $S/a.c:5:12\n    #2 0x5556 in start (/usr/lib/dyld+0x20)\n'
printf 'SUMMARY: AddressSanitizer: heap-buffer-overflow $S/a.c:5:12 in main\n'
printf '$S/t.c:2:5: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type int\n'
printf '==================\nWARNING: ThreadSanitizer: data race (pid=77)\n  Write of size 4 at 0x7b04 by thread T1:\n    #0 f $S/t.c:1:5 (t+0x12)\n  Previous read of size 4 at 0x7b04 by main thread:\n    #0 g $S/t.c:2:5 (t+0x34)\nSUMMARY: ThreadSanitizer: data race $S/t.c:1:5 in f\n'
printf '==9==ERROR: LeakSanitizer: detected memory leaks\n\nDirect leak of 64 byte(s) in 1 object(s) allocated from:\n    #0 0x1 in malloc (/usr/lib/libasan.so+0x9)\n    #1 0x2 in h $S/t.c:3:5\n\nIndirect leak of 8 byte(s) in 1 object(s) allocated from:\n    #0 0x1 in malloc (/usr/lib/libasan.so+0x9)\n    #1 0x3 in main $S/a.c:3:5\nSUMMARY: AddressSanitizer: 72 byte(s) leaked in 2 allocation(s).\n'
exit 1
EOF
printf 'reports:\n\tsh reports.sh\nasan:\n\tcc -g -O0 -fsanitize=address -o a a.c && ./a\n' >"$S/Makefile"
run "$S" "buildrun:make reports;buildwait;problemsdump"
got=$(printf '%s' "$got" | sed -e 's/^button: [^|]*| //' -e 's/Build [0-9]* | //')
check "the sanitizers' reports: where in the program's own code, and what" 'file a.c | >E 5:12 AddressSanitizer: heap-buffer-overflow: READ of size 4 | E 3:5 LeakSanitizer: detected memory leaks: Indirect leak of 8 byte(s) in 1 object(s) | file t.c | E 2:5 runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type int | E 1:5 ThreadSanitizer: data race: Write of size 4 | E 3:5 LeakSanitizer: detected memory leaks: Direct leak of 64 byte(s) in 1 object(s)'
if (cd "$S" && printf 'int main(void){return 0;}\n' | cc -fsanitize=address -x c -o /dev/null - 2>/dev/null); then
    run "$S" "buildrun:make asan;buildwait;problemsdump"
    got=$(printf '%s' "$got" | sed -e 's/^button: [^|]*| //' -e 's/Build [0-9]* | //' -e 's/E 5:[0-9]* /E 5 /')  # (a column, or none, as the symbolizer has it)
    check "a program built with AddressSanitizer, run: its report read" 'file a.c | >E 5 AddressSanitizer: heap-buffer-overflow: READ of size 4'
fi
printf 'one\ntwo\n' >"$dir/notes.txt"
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"
run "$dir/none" "open:$dir/notes.txt;frame;term;frame;text:echo ../notes.txt:2:3: here;key:Return;termwait:here;frame;termclick:notes.txt:2;frame;statusdump"
unset BLIT_TERM_SHELL
got=$(printf '%s' "$got" | sed 's#NORMAL [^ ]*notes.txt#NORMAL notes.txt#')
check "Cmd+click in any terminal (a shell's)" 'NORMAL notes.txt | Ln 2, Col 3 Tabs LF Plain text'

# New projects.
P="$dir/projects"
mkdir -p "$P/taken"
: >"$P/taken/x"
run "$dir/none" "newproject;startdump"
check "the New project tab: C++ first, with its fields" 'new project: kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0'
run "$dir/none" "newproject;key:Down;key:Down;key:Down;key:Down;key:Right;key:Right;projectdump"
check "keys: down to Builds, a choice along" 'kind=C++ name= where=~/Dev git=on | target=Shared library std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true install=true clangd=true tidy=false format=None readme=true configure=true | at 4'
run "$dir/none" "newproject;key:Down;key:Down;key:Down;key:Down;key:Left;projectdump"
check "a field shown only for some values: Install rules, for a library" 'kind=C++ name= where=~/Dev git=on | target=Header-only library std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true install=true clangd=true tidy=false format=None readme=true configure=true | at 4'
run "$dir/none" "newproject;projectcreate;projectwait;projectdump"
check "no name" 'kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0 | error: It needs a name first.'
run "$dir/none" "newproject;project:where=$P;project:name=taken;projectcreate;projectwait;startdump"
check "a folder that is there" 'new project: kind=C++ name=taken where=~/projects git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0 | error: ~/projects/taken is there already.'
run "$dir/none" "newproject;project:where=$P;project:libraries=fmt zlib;project:configure=false;project:git=false;project:name=app;projectcreate;projectwait;frame;statusdump"
check "made: the start screen goes, its main file is open" 'NORMAL src/main.cpp | Ln 1, Col 1 Spaces: 4 LF C++'
got=$(cd "$P/app" && find . -type f | LC_ALL=C sort | tr '\n' ' ')
check "the files: headers in include/, sources in src/" './.clangd ./CMakeLists.txt ./CMakePresets.json ./README.md ./include/app/app.hpp ./src/app.cpp ./src/main.cpp ./tests/test_app.cpp ./vcpkg.json '
got=$(grep -E 'fmt|zlib|TOOLCHAIN_FILE "' "$P/app/CMakeLists.txt" | tr '\n' '|')
check "vcpkg: the toolchain, fmt by its usage notes (the first way), zlib by none" '  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg'"'"'s toolchain")|find_package(fmt CONFIG REQUIRED)|target_link_libraries(app_core PUBLIC fmt::fmt)|# zlib: vcpkg says what to find_package and link (target_link_libraries(app_core PUBLIC ...)) when it installs it.|'
got=$(tr -d ' \n' <"$P/app/vcpkg.json")
check "vcpkg.json" '{"name":"app","version-string":"0.1.0","dependencies":["fmt","zlib"]}'
run "$dir/none" "newproject;project:where=$P;project:kind=C;project:build=Make;project:name=cm;projectcreate;projectwait;buildwait;builddump"
check "C with make: made, git init run, and the Build tab has it" 'Make: >make make all make run make test make release make clean (last: 0)'
[ -d "$P/cm/.git" ] && got=git || got='no .git'
check "git init" 'git'
run "$P/cm" "buildrun:make test;buildwait;builddump"
check "and it builds, and its test passes" 'Make: make make all make run >make test make release make clean (last: 0)'
run "$dir/none" "newproject;project:where=$P;project:kind=Python;project:manager=pip;project:type=Script;project:install=false;project:name=tool;projectcreate;projectwait;projectdump"
check "Python: a script with pip" 'kind=Python name=tool where=~/projects git=on | manager=pip type=Script python=3.13 backend_pip=hatchling deps= ruff=true types=mypy readme=true install=false | at 0 | made ~/projects/tool'

printf 'notes\n' >"$dir/notes.txt"
run "$dir/none" "open:$dir/notes.txt;frame;key:cmd+alt+n;frame;startdump"
check "New project (Cmd+Option+N) once a file is open: the form, in a tab of its own" 'new project: kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0'
run "$dir/none" "open:$dir/notes.txt;frame;key:cmd+alt+n;frame;key:Escape;frame;startdump"
got=${got%% o:*}
check "and Esc there: the start screen's Start tab" 'start >n:New file p:New project…'

# init.lua's own kinds, and one taken away.
cat >"$dir/kind.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.project.add("Notes", {description = "Markdown notes",
  fields = {{key = "title", label = "Title", default = "untitled"}, {key = "toc", label = "Contents", kind = "switch", default = true}},
  create = function(dir, v)
    blit.fs.write(dir .. "/notes.md", "# " .. v.title .. (v.toc and "\n\n[TOC]\n" or "\n"))
    return "notes.md"
  end})
blit.project.remove("Python")
EOF
INIT="$dir/kind.lua"
run "$dir/none" "newproject;project:where=$P;project:kind=Python;projectdump"
check "a kind taken away is not there" 'kind=C++ name= where=~/projects git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0'
INIT="$dir/kind.lua"
run "$dir/none" "newproject;project:where=$P;project:kind=Notes;project:title=Ideas;project:git=false;project:name=notes;projectcreate;projectwait;frame;statusdump"
check "blit.project.add: its create made it, and its file is open" 'NORMAL notes.md | Ln 1, Col 1 Tabs LF Markdown'
got=$(tr '\n' '|' <"$P/notes/notes.md")
check "with the values set" '# Ideas||[TOC]|'

# Run configurations: .blit/run.json beside the project, run by the Run
# button (its build first), and what Debug and Profile run.
R="$dir/rc"
mkdir -p "$R/.blit" "$R/build"
cat >"$R/Makefile" <<'EOF'
all:
	@printf '#!/bin/sh\necho args: "$$*" greeting: "$$GREETING" in: "$$(pwd)"\n' > build/hello && chmod +x build/hello && echo built
EOF
cat >"$R/.blit/run.json" <<'EOF'
{ "configurations": [
  { "name": "hello", "program": "build/hello", "args": ["one", "two words"], "env": { "GREETING": "hi there" }, "cwd": "build", "build": "make" },
  { "name": "plain", "program": "build/hello", "args": "x y" } ] }
EOF
INIT=
run "$R" "builddump"
check "run configurations, before what the folder builds with; the first is the Run button's" 'Run: >hello plain | Make: make make all'
run "$R" "runconfdump"
check "Debug and Profile take its program, arguments, folder and environment" "hello | debug: hello one 'two words' in build GREETING=hi there | profile: hello one 'two words' in build GREETING=hi there"
run "$R" "buildrun;buildwait;builddump"
check "Run: its build, then it" 'Run: >hello plain | Make: make make all (last: 0)'
if test -x "$R/build/hello"; then got=built; else got=missing; fi
check "(the build ran first)" 'built'
run "$R" "buildrun:plain;buildwait;runconfdump"
check "another chosen: theirs too" 'plain | debug: hello x y in rc | profile: hello x y in rc'
printf '{ "configurations": [ { "name": "x" } ] }\n' >"$R/.blit/run.json"
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$R;frame;builddump" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "a configuration without its program: said" 'blit: .blit/run.json: a configuration needs a "name" and a "program"'
mkdir -p "$dir/fresh"
printf 'all:\n\ttrue\n' >"$dir/fresh/Makefile"
run "$dir/fresh" "command:Edit the run configurations;frame;lsptext"
check "Edit the run configurations: made, with one to fill in" '{ /   "configurations": [ /     { /       "name": "fresh", /       "program": "build/fresh", /       "args": [], /       "env": {}, /       "cwd": "." /     } /   ] / } / '

# A file's assembly beside it: from its own command in a build folder's
# compile_commands.json (its -D is needed), linked line by line, and made
# again when the file is saved; with no such command, the plain compiler's.
A="$dir/asm"
mkdir -p "$A/build" "$A/plain"
printf '// sums\nint sum(const int* a, int n) {\n    int s = 0;\n    for (int i = 0; i < n; ++i) s += a[i];\n    return s;\n}\n#ifndef STEP\n#error no STEP\n#endif\n' >"$A/sum.c"
printf '[{"directory": "%s", "command": "cc -O0 -DSTEP=1 -c sum.c -o build/sum.o", "file": "sum.c"}]\n' "$A" >"$A/build/compile_commands.json"
run "$A" "open:$A/sum.c;command:Show the assembly beside the file;asmdump"
check "a comment's line: none of the assembly" '1:'
first=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$A;open:$A/sum.c;command:Show the assembly beside the file;key:Down;key:Down;key:Down;asmdump;vim:O<Esc>j;save;asmdump" 2>&1 >/dev/null | tail -2 | tr '\n' '|')
loop=${first%%|*}
case $loop in "4: "[0-9]*) got=lines ;; *) got=$loop ;; esac
check "the loop's line: its instructions" 'lines'
moved=${first#*|}
got=${moved%|}
check "a line put above it and saved: made again, the same instructions now the next line's" "5:${loop#4:}"
printf 'int twice(int x) { return 2 * x; }\n' >"$A/plain/two.c"
run "$A/plain" "open:$A/plain/two.c;command:Show the assembly beside the file;asmdump"
case $got in "1: "[0-9]*) got=lines ;; esac
check "no compile_commands.json: the plain compiler's" 'lines'
printf 'int broken( {\n' >"$A/plain/bad.c"
run "$A/plain" "open:$A/plain/bad.c;command:Show the assembly beside the file;asmdump"
case $got in *error*) got=said ;; esac
check "a file that does not compile: the compiler's errors in the tab" 'said'

exit $failed
