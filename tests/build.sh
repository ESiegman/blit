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
run "$dir/none" "newproject;projectcreate;projectdump"
check "no name" 'kind=C++ name= where=~/Dev git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0 | error: It needs a name first.'
run "$dir/none" "newproject;project:where=$P;project:name=taken;projectcreate;startdump"
check "a folder that is there" 'new project: kind=C++ name=taken where=~/projects git=on | target=Program std=23 libraries= tests=CTest warnings=Standard sanitizers=Address + undefined tuning=Portable lto=false generator=Ninja presets=true clangd=true tidy=false format=None readme=true configure=true | at 0 | error: ~/projects/taken is there already.'
run "$dir/none" "newproject;project:where=$P;project:libraries=fmt zlib;project:configure=false;project:git=false;project:name=app;projectcreate;frame;statusdump"
check "made: the start screen goes, its main file is open" 'src/main.cpp | Ln 1, Col 1 Spaces: 4 LF C++'
got=$(cd "$P/app" && find . -type f | sort | tr '\n' ' ')
check "the files: headers in include/, sources in src/" './.clangd ./CMakeLists.txt ./CMakePresets.json ./README.md ./include/app/app.hpp ./src/app.cpp ./src/main.cpp ./tests/test_app.cpp ./vcpkg.json '
got=$(grep -E 'fmt|zlib|TOOLCHAIN_FILE "' "$P/app/CMakeLists.txt" | tr '\n' '|')
check "vcpkg: the toolchain, fmt by its usage notes (the first way), zlib by none" '  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg'"'"'s toolchain")|find_package(fmt CONFIG REQUIRED)|target_link_libraries(app_core PUBLIC fmt::fmt)|# zlib: vcpkg says what to find_package and link (target_link_libraries(app_core PUBLIC ...)) when it installs it.|'
got=$(tr -d ' \n' <"$P/app/vcpkg.json")
check "vcpkg.json" '{"name":"app","version-string":"0.1.0","dependencies":["fmt","zlib"]}'
run "$dir/none" "newproject;project:where=$P;project:kind=C;project:build=Make;project:name=cm;projectcreate;buildwait;builddump"
check "C with make: made, git init run, and the Build tab has it" 'Make: >make make all make run make test make release make clean (last: 0)'
[ -d "$P/cm/.git" ] && got=git || got='no .git'
check "git init" 'git'
run "$P/cm" "buildrun:make test;buildwait;builddump"
check "and it builds, and its test passes" 'Make: make make all make run >make test make release make clean (last: 0)'
run "$dir/none" "newproject;project:where=$P;project:kind=Python;project:manager=pip;project:type=Script;project:install=false;project:name=tool;projectcreate;projectdump"
check "Python: a script with pip" 'kind=Python name=tool where=~/projects git=on | manager=pip type=Script python=3.13 backend_pip=hatchling deps= ruff=true types=mypy readme=true install=false | at 0 | made ~/projects/tool'

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
run "$dir/none" "newproject;project:where=$P;project:kind=Notes;project:title=Ideas;project:git=false;project:name=notes;projectcreate;frame;statusdump"
check "blit.project.add: its create made it, and its file is open" 'notes.md | Ln 1, Col 1 Tabs LF Markdown'
got=$(tr '\n' '|' <"$P/notes/notes.md")
check "with the values set" '# Ideas||[TOC]|'

exit $failed
