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
[ $failed = 0 ] && echo "container ok"
exit $failed
