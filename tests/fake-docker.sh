#!/bin/sh
# Stands in for docker in the tests (BLIT_DOCKER names it). Each call is
# written to $BLIT_FAKE_DOCKER_LOG; containers are files in
# $BLIT_FAKE_DOCKER_STATE saying whether they run; "exec" runs the command
# here, in a home of its own ($BLIT_FAKE_HOME), so that blit's agent is
# copied there and run as it would be in a container.
echo "$*" >>"${BLIT_FAKE_DOCKER_LOG:-/dev/null}"
state=${BLIT_FAKE_DOCKER_STATE:?}
mkdir -p "$state"
what=$1
shift
case "$what" in
    inspect)
        while [ $# -gt 1 ]; do shift; done
        [ -f "$state/$1" ] || { echo "Error: No such object: $1" >&2; exit 1; }
        cat "$state/$1" ;;
    build) [ -z "${BLIT_FAKE_DOCKER_FAIL:-}" ] || { echo "build failed: $BLIT_FAKE_DOCKER_FAIL" >&2; exit 1; } ;;
    run)
        name=
        while [ $# -gt 0 ]; do
            case "$1" in
                --name) name=$2; shift 2 ;;
                -d) shift ;;
                *) shift ;;
            esac
        done
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
