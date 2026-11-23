#!/bin/sh
# Dev containers, with tests/fake-docker.sh standing in for docker: the
# container made from devcontainer.json (its image, or its Dockerfile
# built), started again when it is there, postCreateCommand run once, and
# its workspace opened through docker exec, blit's agent copied in as over
# ssh. No real docker is ever asked anything.
#
#   tests/container.sh path/to/blit
set -u
blit=${1:?usage: tests/container.sh path/to/blit}
here=$(cd "$(dirname "$0")" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_DOCKER="$here/fake-docker.sh" BLIT_FAKE_DOCKER_STATE="$dir/state" BLIT_FAKE_DOCKER_LOG="$dir/log"
export BLIT_FAKE_HOME="$dir/home" BLIT_RECENT_FILE="$dir/recent" XDG_DATA_HOME="$dir/data"
# path_without PROGRAM...: a PATH of /usr/bin's and /bin's programs but not
# those named (a machine they are not installed on), and what blit calls
# this machine.
path_without() {
    p="$dir/without-$(printf '%s' "$*" | tr ' ' -)"
    mkdir -p "$p"
    ln -s /usr/bin/* "$p/" 2>/dev/null
    ln -s /bin/* "$p/" 2>/dev/null
    for n in "$@"; do rm -f "$p/$n"; done
    printf '%s' "$p"
}
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
failed=0
fail() {
    printf 'FAIL: %s\n%s\n' "$1" "$(printf '%s\n' "$2" | sed 's/^/    /')"
    failed=1
}
# fresh JSON: the folder, with this devcontainer.json; no containers.
fresh() {
    rm -rf "$W" "$dir/state" "$dir/log" "$dir/home"
    mkdir -p "$W/.devcontainer" "$dir/home"
    echo hi >"$W/notes.txt"
    printf '%s\n' "$1" >"$W/.devcontainer/devcontainer.json"
    : >"$dir/log"
}
# run: the command, then what was shown; the container's name as NAME.
run() {
    "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "command:$1;luawait;remotewait;treewait;frame;statusdump;treedump" "$W" 2>&1 >/dev/null |
        sed -e 's/blit-work-[0-9a-f]*/NAME/g' -e "s#$dir#@#g" -e 's/@[0-9][0-9]*//g'
}
log() { sed -e 's/blit-work-[0-9a-f]*/NAME/g' -e "s#$dir#@#g" "$dir/log" | cut -d' ' -f1-2 | tr '\n' '|'; }

json='// comments and trailing commas (a comment after one too), as devcontainer.json may have
{
  "image": "ubuntu:24.04", /* the image */
  "workspaceFolder": "'"$W"'",
  "containerEnv": { "FOO": "bar" },
  "runArgs": [
    "--cap-add=SYS_PTRACE", // to debug
  ],
  "postCreateCommand": "echo made >> made.txt",
}'
fresh "$json"
got=$(run "Open in the dev container")
printf '%s' "$got" | grep -q '^docker:NAME ' || fail "made, started and opened: its workspace through docker exec" "$got"
printf '%s' "$got" | grep -q '^tree: .devcontainer/ made.txt notes.txt$' || fail "the container's files in the tree" "$got"
[ "$(log)" = "inspect -f|run -d|exec -w|exec -i|" ] || fail "asked of docker: is it there, make it, postCreateCommand, the agent" "$(log)"
grep -q -- "--mount type=bind,source=$W,target=$W -w $W -e FOO=bar --cap-add=SYS_PTRACE ubuntu:24.04 " "$dir/log" || fail "docker run: the mount, the folder, the variables, runArgs, the image" "$(cat "$dir/log")"
[ "$(cat "$W/made.txt")" = made ] || fail "postCreateCommand: run in the workspace" "$(ls "$W")"
# Again: there and running, opened as it is.
: >"$dir/log"
got=$(run "Open in the dev container")
[ "$(log)" = "inspect -f|exec -i|" ] || fail "running already: opened, nothing made" "$(log)"
[ "$(cat "$W/made.txt")" = made ] || fail "postCreateCommand: once" "$(cat "$W/made.txt")"
# Stopped: started.
for f in "$dir/state"/*; do echo false >"$f"; done
: >"$dir/log"
got=$(run "Open in the dev container")
[ "$(log)" = "inspect -f|start NAME|exec -i|" ] || fail "stopped: started again" "$(log)"
# Rebuilt: the one there goes, a new one is made.
: >"$dir/log"
got=$(run "Rebuild the dev container")
[ "$(log)" = "rm -f|run -d|exec -w|exec -i|" ] || fail "rebuilt: removed and made anew" "$(log)"
printf '%s' "$got" | grep -q '^docker:NAME ' || fail "rebuilt: opened" "$got"

# A trailing comma with what is commented out after it (as the templates
# leave it): still JSON.
fresh '{
  "image": "ubuntu:24.04",
  "workspaceFolder": "'"$W"'",
  // "features": {},
  /* "forwardPorts": [] */
}'
got=$(run "Open in the dev container")
printf '%s' "$got" | grep -q '^docker:NAME ' || fail "a trailing comma before comments: opened" "$got"

# A Dockerfile: built first, its context beside it unless given.
fresh '{ "build": { "dockerfile": "Dockerfile" }, "workspaceFolder": "'"$W"'" }'
got=$(run "Open in the dev container")
grep -q "^build -t NAME-image -f $W/.devcontainer/Dockerfile $W/.devcontainer\$" "$(sed 's/blit-work-[0-9a-f]*/NAME/g' "$dir/log" >"$dir/log2"; echo "$dir/log2")" || fail "a Dockerfile: built" "$(cat "$dir/log")"
grep -q " NAME-image /bin/sh" "$dir/log2" || fail "a Dockerfile: its image run" "$(cat "$dir/log2")"
printf '%s' "$got" | grep -q '^docker:NAME ' || fail "a Dockerfile: opened" "$got"
export BLIT_FAKE_DOCKER_FAIL="no such base image"
rm -rf "$dir/state"
got=$(run "Open in the dev container")
printf '%s' "$got" | grep -q "blit: The dev container's image did not build: build failed: no such base image" || fail "a build that fails: said" "$got"
unset BLIT_FAKE_DOCKER_FAIL
# docker's daemon not running: said so, not that the image did not build,
# both opening and rebuilding.
export BLIT_FAKE_DOCKER_DOWN=1
for what in "Open in the dev container" "Rebuild the dev container"; do
    got=$(run "$what")
    printf '%s' "$got" | grep -q '^blit: docker is not running: Cannot connect to the Docker daemon' || fail "docker not running ($what): said" "$got"
    printf '%s' "$got" | grep -q 'did not build\|did not start' && fail "docker not running ($what): nothing else said" "$got"
done
unset BLIT_FAKE_DOCKER_DOWN
# No docker on this machine at all: said so, both opening and rebuilding
# (and not that the image did not build).
for what in "Open in the dev container" "Rebuild the dev container"; do
    got=$(BLIT_DOCKER= PATH=$(path_without docker) run "$what")
    printf '%s' "$got" | grep -qF "blit: docker is not installed on $this" || fail "docker not installed ($what): said" "$got"
    printf '%s' "$got" | grep -q 'did not build\|did not start\|cannot run' && fail "docker not installed ($what): nothing else said" "$got"
done

# A folder with a comma (and a quote) in its path: mounted as docker reads
# --mount (CSV), the fields with them in quotes; and the same for a
# workspaceMount that names the folder.
W="$dir/one, two \"three\""
Wj=$(printf '%s' "$W" | sed 's/"/\\"/g')
export BLIT_FAKE_DOCKER_MOUNT="$dir/mount"
for mount in "" '"workspaceMount": "source=${localWorkspaceFolder},target=${containerWorkspaceFolder},type=bind",'; do
    fresh '{ "image": "ubuntu:24.04", '"$mount"' "workspaceFolder": "'"$Wj"'" }'
    rm -f "$dir/mount"
    got=$(run "Open in the dev container")
    printf '%s' "$got" | grep -q '^docker:blit-one--two--three-' || fail "a comma in the path${mount:+ (workspaceMount)}: opened" "$got"
    grep -qx "source=$W" "$dir/mount" && grep -qx "target=$W" "$dir/mount" && grep -qx "type=bind" "$dir/mount" ||
        fail "a comma in the path${mount:+ (workspaceMount)}: mounted whole" "$(cat "$dir/mount" 2>/dev/null; cat "$dir/log")"
done
unset BLIT_FAKE_DOCKER_MOUNT
W="$dir/work"

# Not there, or of no use.
rm -rf "$W/.devcontainer"
got=$(run "Open in the dev container")
printf '%s' "$got" | grep -q "blit: No devcontainer.json in @/work" || fail "no devcontainer.json: said" "$got"
fresh '{ "name": "nothing" }'
got=$(run "Open in the dev container")
printf '%s' "$got" | grep -q "names no image and no Dockerfile" || fail "no image: said" "$got"

# A file in a running container, by its path.
fresh "$json"
mkdir -p "$dir/state"
echo true >"$dir/state/box"
got=$("$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "remotewait;frame;statusdump;docdump:0" "docker://box$W/notes.txt" 2>&1 >/dev/null | sed "s#$dir#@#g")
printf '%s' "$got" | grep -q '^docker:box .*/work/notes.txt ' || fail "docker://box/path: opened there" "$got"
printf '%s' "$got" | grep -q '^> |hi$' || fail "docker://box/path: its text" "$got"

# No server for the language in it (three are named): said once, not once for each.
none="blit.opt.lsp.servers.clangd = { enabled = false }"
for n in 1 2 3; do none="$none
blit.opt.lsp.servers.none$n = { command = { \"blit-no-such-server-$n\" }, languages = { \"cpp\" } }"; done
printf 'blit.opt.git.enabled = false\nblit.opt.editor.modal = false\n%s\n' "$none" >"$dir/none.lua"
printf 'int a;\n' >"$W/a.cpp"
got=$(BLIT_INIT="$dir/none.lua" "$blit" --offscreen 1200x700 --shot "$dir/shot.bmp" --script "remotewait;frame;lspsettle;wait:300;frame;lspsettle;wait:300;frame;lspsettle" "docker://box$W/a.cpp" 2>&1 >/dev/null)
[ "$got" = "blit: language server: no language server for this file is installed on docker:box
blit: No language server for C++: none of none1, none2 or none3 is installed on docker:box (lsp.servers in Settings)" ] || fail "no server in it: said once (and which, in a toast)" "$got"
[ $failed = 0 ] && echo "container ok"
exit $failed
