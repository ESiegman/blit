#!/bin/sh
# Stands in for docker in the tests (BLIT_DOCKER names it). Each call is
# written to $BLIT_FAKE_DOCKER_LOG; containers are files in
# $BLIT_FAKE_DOCKER_STATE saying whether they run; "run" reads its --mount
# as docker does (and refuses one it could not); "exec" runs the command
# here, in a home of its own ($BLIT_FAKE_HOME), so that blit's agent is
# copied there and run as it would be in a container.
echo "$*" >>"${BLIT_FAKE_DOCKER_LOG:-/dev/null}"
# BLIT_FAKE_DOCKER_DOWN: its daemon is not running, and every call says so.
[ -z "${BLIT_FAKE_DOCKER_DOWN:-}" ] || { echo "Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?" >&2; exit 1; }
state=${BLIT_FAKE_DOCKER_STATE:?}
mkdir -p "$state"
what=$1
shift
case "$what" in
    info) echo 27.0.0 ;;
    inspect)
        while [ $# -gt 1 ]; do shift; done
        [ -f "$state/$1" ] || { echo "Error: No such object: $1" >&2; exit 1; }
        cat "$state/$1" ;;
    build) [ -z "${BLIT_FAKE_DOCKER_FAIL:-}" ] || { echo "build failed: $BLIT_FAKE_DOCKER_FAIL" >&2; exit 1; } ;;
    run)
        name= mount=
        while [ $# -gt 0 ]; do
            case "$1" in
                --name) name=$2; shift 2 ;;
                --mount) mount=$2; shift 2 ;;
                -d) shift ;;
                *) shift ;;
            esac
        done
        if [ -n "$mount" ]; then
            # Read as docker reads it: CSV (a field in double quotes may
            # hold commas, and "" in it is a quote), each field KEY=VALUE.
            # Its fields, a line each, go to $BLIT_FAKE_DOCKER_MOUNT.
            fields=$(printf '%s\n' "$mount" | awk '{
                s = $0; n = length(s); f = ""; st = 0; first = 1; bad = 0
                for (i = 1; i <= n; i++) {
                    c = substr(s, i, 1)
                    if (st == 1) {
                        if (c != "\"") f = f c
                        else if (substr(s, i + 1, 1) == "\"") { f = f c; i++ }
                        else st = 2
                    } else if (c == ",") { print f; f = ""; st = 0; first = 1; continue }
                    else if (st == 2) bad = 1
                    else if (c == "\"") { if (first) st = 1; else bad = 1 }
                    else f = f c
                    first = 0
                }
                print f
                exit (bad || st == 1)
            }') || { echo "invalid argument \"$mount\" for \"--mount\" flag: parse error" >&2; exit 125; }
            type= source=
            while IFS= read -r f; do
                case "$f" in
                    type=*) type=${f#*=} ;;
                    source=*|src=*) source=${f#*=} ;;
                    target=*|dst=*|destination=*|readonly|ro|readonly=*|ro=*|consistency=*|bind-*) ;;
                    *=*) echo "invalid argument \"$mount\" for \"--mount\" flag: unexpected key '${f%%=*}' in '$f'" >&2; exit 125 ;;
                    *) echo "invalid argument \"$mount\" for \"--mount\" flag: invalid field '$f' must be a key=value pair" >&2; exit 125 ;;
                esac
            done <<FIELDS
$fields
FIELDS
            [ "$type" = bind ] && [ ! -d "$source" ] && { echo "docker: Error response from daemon: invalid mount config for type \"bind\": bind source path does not exist: $source" >&2; exit 125; }
            printf '%s\n' "$fields" >>"${BLIT_FAKE_DOCKER_MOUNT:-/dev/null}"
        fi
        [ -f "$state/$name" ] && { echo "docker: Conflict. The container name \"/$name\" is already in use." >&2; exit 125; }
        echo true >"$state/$name"
        echo 0123456789abcdef ;;
    start) [ -f "$state/$1" ] || exit 1; echo true >"$state/$1" ;;
    rm)
        [ "$1" = -f ] && shift
        rm -f "$state/$1" ;;
    exec)
        while [ $# -gt 0 ]; do
            case "$1" in
                -i) shift ;;
                -w) cd "$2" || exit 126; shift 2 ;;
                -*) shift 2 ;;
                *) break ;;
            esac
        done
        name=$1
        shift
        [ "$(cat "$state/$name" 2>/dev/null)" = true ] || { echo "Error: container $name is not running" >&2; exit 1; }
        [ -n "${BLIT_FAKE_HOME:-}" ] && HOME=$BLIT_FAKE_HOME && export HOME
        exec "$@" ;;
    *) echo "fake docker: $what?" >&2; exit 1 ;;
esac
