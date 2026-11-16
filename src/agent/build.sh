#!/bin/sh
# Builds blit's agent (blit-agent.cpp, with the finder it uses) for Linux
# servers, x86_64 and arm64: with GCC 16, the compiler blit itself is built
# with, in Docker's official gcc:16 image, and linked statically so that it
# runs on any Linux whatever its C library. The binaries go in bin/ beside
# this script and are kept with the sources (as the compiled shaders are), so
# Docker is needed only when the agent changes; blit embeds them.
#
#   src/agent/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
mkdir -p "$here/bin"
for pair in amd64:x86_64 arm64:aarch64; do
    platform=${pair%%:*}
    name=${pair##*:}
    echo "blit-agent for Linux $name"
    docker run --rm --platform "linux/$platform" -v "$src:/src:ro" -v "$here/bin:/out" gcc:16 \
        g++ -std=c++26 -O2 -static -s -pthread -DBLIT_AGENT=1 -I/src -o "/out/blit-agent-linux-$name" \
            /src/agent/blit-agent.cpp /src/finder.cpp /src/fuzzy.cpp /src/pty.cpp
done
ls -l "$here/bin"
