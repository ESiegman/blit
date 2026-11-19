#!/bin/sh
# blit's Linux release, built on AlmaLinux 9 so that what it makes runs on
# any Linux with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora 36 and
# newer) and on RHEL 9 and its rebuilds (2.34, with the one 2.35 function
# GCC's unwinder asks for, _dl_find_object, brought back). Run inside an almalinux:9 container, from the top of
# the sources:
#
#   docker run --rm --network host -v "$PWD:/src" -w /src almalinux:9 packaging/linux/build.sh 0.2.0
#
# GCC 16 is Red Hat's toolset (libstdc++ linked in, as on every Linux
# build). SDL3, Lua 5.5 and tree-sitter are built from their sources, at the
# versions blit is tested with: Lua and tree-sitter are linked in; SDL3 and
# the picture readers (libpng, libjpeg-turbo, giflib, libwebp) are carried
# beside the program. glibc, the Vulkan loader, fontconfig and FreeType are
# the system's, and so are librsvg and poppler, which blit loads only when
# an SVG or a PDF is opened. Leaves
# out/blit-VERSION-x86_64.AppImage, out/blit-VERSION-linux-x86_64.tar.gz,
# their SHA-256 and out/linux-glibc.txt (the newest glibc symbol it uses).
set -eu
version=${1:?usage: packaging/linux/build.sh VERSION}
SDL=release-3.4.20
LUA=5.5.1
TREE_SITTER=v0.26.9
VULKAN_HEADERS=vulkan-sdk-1.4.363.0
deps=/opt/blit-deps
top=$(pwd)
work=$(mktemp -d)

dnf -q -y install dnf-plugins-core
dnf config-manager --set-enabled crb
dnf -q -y install gcc-toolset-16-gcc-c++ gcc-toolset-16-libstdc++-devel gcc-toolset-16-binutils \
    git file make diffutils tar gzip findutils python3-pip \
    freetype-devel fontconfig-devel zlib-devel vulkan-loader-devel \
    libpng-devel turbojpeg-devel giflib-devel libwebp-devel cairo-devel librsvg2-devel poppler-glib-devel \
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel libXfixes-devel libXtst-devel \
    libxkbcommon-devel wayland-devel wayland-protocols-devel mesa-libEGL-devel libdecor-devel
pip3 install -q 'cmake>=3.28' ninja
tools=/opt/rh/gcc-toolset-16/root/usr/bin
export PATH="/usr/local/bin:$tools:$PATH" CC="$tools/gcc" CXX="$tools/g++"

# SDL3: the video and events blit uses (no sound, camera or joysticks to bring their libraries along).
git clone -q --depth 1 -b "$SDL" https://github.com/libsdl-org/SDL.git "$work/SDL"
cmake -S "$work/SDL" -B "$work/SDL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_POWER=OFF
cmake --build "$work/SDL/build"
cmake --install "$work/SDL/build"

# Lua 5.5, a static library.
curl -sSfL "https://www.lua.org/ftp/lua-$LUA.tar.gz" | tar -xz -C "$work"
make -C "$work/lua-$LUA" -s linux CC="$CC" MYCFLAGS=-fPIC
make -C "$work/lua-$LUA" -s install INSTALL_TOP="$deps"

# tree-sitter, a static library.
git clone -q --depth 1 -b "$TREE_SITTER" https://github.com/tree-sitter/tree-sitter.git "$work/tree-sitter"
make -C "$work/tree-sitter" -s CC="$CC" PREFIX="$deps" install
rm -f "$deps"/lib/libtree-sitter.so*

# Vulkan's headers as new as blit's backend asks for (the loader is the system's).
git clone -q --depth 1 -b "$VULKAN_HEADERS" https://github.com/KhronosGroup/Vulkan-Headers.git "$work/Vulkan-Headers"
cmake -S "$work/Vulkan-Headers" -B "$work/Vulkan-Headers/build" -DCMAKE_INSTALL_PREFIX="$deps" >/dev/null
cmake --install "$work/Vulkan-Headers/build" >/dev/null

# blit.
cmake -S "$top" -B "$work/blit" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBLIT_NATIVE=OFF \
    -DCMAKE_PREFIX_PATH="$deps" -DVulkan_INCLUDE_DIR="$deps/include" -DBLIT_VERSION="$version"
cmake --build "$work/blit"
rm -rf "$work/AppDir"
DESTDIR="$work/AppDir" cmake --install "$work/blit"

# The AppImage, SDL3 carried inside it (and beside the program in the tarball).
curl -sSfL -o "$work/linuxdeploy" https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
chmod +x "$work/linuxdeploy"
cd "$work"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 LINUXDEPLOY_OUTPUT_VERSION="$version" LD_LIBRARY_PATH="$deps/lib" ./linuxdeploy --appdir AppDir \
    --executable AppDir/usr/bin/blit \
    --desktop-file AppDir/usr/share/applications/blit.desktop \
    --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/blit.png \
    --exclude-library 'libvulkan.so*' \
    --exclude-library 'libglib-2.0.so*' --exclude-library 'libpcre*.so*' --exclude-library 'libgraphite2.so*' \
    --exclude-library 'libxml2.so*' --exclude-library 'liblzma.so*' --exclude-library 'libbz2.so*' --exclude-library 'libbrotli*.so*' \
    --output appimage
# Only what few systems have, or have at the version blit was built with:
# what the system's FreeType, HarfBuzz and fontconfig need is the system's
# (an older GLib beside blit would be the one its dlopen() of librsvg and
# poppler found, under the system's newer ones).
for f in AppDir/usr/lib/*; do
    case "${f##*/}" in
        libSDL3.so* | libpng16.so* | libgif.so* | libturbojpeg.so* | libwebp.so* | libwebpdemux.so*) ;;
        *) echo "not meant to be carried: ${f##*/}" >&2; exit 1 ;;
    esac
    # (and each with its license in the notices)
    stem=${f##*/}
    stem=${stem%%.so*}
    grep -q "\`$stem\`" "$top/THIRD_PARTY_NOTICES.md" || { echo "carried without its notice in THIRD_PARTY_NOTICES.md: $stem" >&2; exit 1; }
done
ls AppDir/usr/share/doc/blit/LICENSE AppDir/usr/share/doc/blit/NOTICE AppDir/usr/share/doc/blit/THIRD_PARTY_NOTICES.md >/dev/null

mkdir -p "$top/out"
dir="blit-$version-linux-x86_64"
rm -rf "$dir" && mkdir "$dir"
cp -a AppDir/usr/bin AppDir/usr/lib AppDir/usr/share "$dir/"
tar -czf "$top/out/$dir.tar.gz" "$dir"
mv ./blit*.AppImage "$top/out/blit-$version-x86_64.AppImage"
for f in AppDir/usr/bin/blit AppDir/usr/lib/*.so*; do objdump -T "$f" | grep -o 'GLIBC_[0-9.]*'; done | sort -uV | tail -1 > "$top/out/linux-glibc.txt"
(cd "$top/out" && for f in blit-*; do case "$f" in *.sha256) ;; *) sha256sum "$f" > "$f.sha256" ;; esac; done)
ls -l "$top/out" AppDir/usr/lib
echo "needs $(cat "$top/out/linux-glibc.txt")"
