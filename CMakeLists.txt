cmake_minimum_required(VERSION 3.28)
# The macOS the build is for: the one it is built on, unless said otherwise,
# which is also what Homebrew's libraries are built for. Left alone, GCC aims
# at the major version it was itself built on (27.0) and Apple's clang at the
# running one (27.2), and the link warned of every object and library the
# other had made. (Set before project(): it is read as the languages start.)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  execute_process(COMMAND sw_vers -productVersion OUTPUT_VARIABLE BLIT_MACOS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(BLIT_MACOS MATCHES "^([0-9]+\\.[0-9]+)")
    # (FORCE: a build folder made before this has the entry, empty, and a
    # cache entry is otherwise never set over. Only an empty one gets here.)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_MATCH_1}" CACHE STRING "The macOS version the build is for" FORCE)
  endif()
endif()
project(blit VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BLIT_NATIVE "Tune for the build machine's CPU" ON)
option(BLIT_LTO "Link-time optimization" ON)
option(BLIT_STATIC_RUNTIME "Link libstdc++ and libgcc statically (faster process start)" ON)
option(BLIT_OPTIMIZE_DEBUG "Compile a Debug build with Release's optimization, for benchmarking" OFF)
option(BLIT_SANITIZE "Compile a Debug build with AddressSanitizer, UndefinedBehaviorSanitizer and libstdc++'s checks, for the tests" OFF)

# Tests and benchmarks are development tools, and only a Debug build has
# them: the test program, the benchmark tools, and the editor's own benchmark
# and screenshot modes (--bench, --shot, --script). A Release build is the
# editor and nothing else.
#
# A plain Debug build is unoptimized, which is right for a debugger and for
# the tests but makes benchmark numbers meaningless. For benchmarking,
# configure the Debug build with -DBLIT_OPTIMIZE_DEBUG=ON: same code
# generation as Release, with the development tools. bench/run.sh expects
# that build in build-bench/.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(BLIT_DEV ON)
else()
  set(BLIT_DEV OFF)
endif()

# (GCC's flags are for the C++: the Metal side is Objective-C, built by
# Apple's compiler, below.)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wshadow;-fno-exceptions;-fno-rtti>")
# A Debug build warns of more that is likely a mistake: an else-if whose
# condition is one already tested, an if (or a ?:) whose two branches are
# the same, && and || that cannot be what was meant, a format that is not
# a literal, a virtual function overridden without `override`, const cast
# away, an array sized at run time. The code has none of them; this keeps
# it so. (GCC's names for them.)
if(BLIT_DEV AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op;-Wformat=2;-Wsuggest-override;-Wcast-qual;-Wvla>")
endif()
# A Debug build that checks itself as it runs (-DBLIT_SANITIZE=ON):
# memory read or written out of bounds or after it was freed, overflows
# and other undefined behaviour (each stops the program), an index past a
# vector's end. Slower, with larger stack frames: for the tests, not the
# benchmarks. The C++ only: the Objective-C side and the allocation
# sampler (a library of its own, put into other programs) are as they are.
if(BLIT_DEV AND BLIT_SANITIZE)
  set(blit_sanitize -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS -D_GLIBCXX_SANITIZE_VECTOR)
  if(NOT BLIT_OPTIMIZE_DEBUG)
    # (-O1, as the sanitizers' makers advise: unoptimized, the checked
    # program is slow enough that the tests that go by time fail.)
    list(APPEND blit_sanitize -O1)
  endif()
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${blit_sanitize}>")
  add_link_options("$<$<LINK_LANGUAGE:CXX>:-fsanitize=address,undefined>")
endif()
if(BLIT_NATIVE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    # On macOS GCC cannot tell which Apple chip it is on, and -mcpu=native
    # gives the first one (apple-m1). The chip's name says which.
    set(BLIT_CPU native)
    if(APPLE)
      execute_process(COMMAND sysctl -n machdep.cpu.brand_string OUTPUT_VARIABLE BLIT_CHIP OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(BLIT_CHIP MATCHES "Apple (M[0-9]+)")
        string(TOLOWER "apple-${CMAKE_MATCH_1}" BLIT_CHIP_CPU)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-mcpu=${BLIT_CHIP_CPU}" BLIT_HAS_CHIP_CPU)
        if(BLIT_HAS_CHIP_CPU)
          set(BLIT_CPU ${BLIT_CHIP_CPU})
        endif()
      endif()
    endif()
    message(STATUS "blit: -mcpu=${BLIT_CPU}")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-mcpu=${BLIT_CPU}>")
  else()
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-march=native>")
  endif()
endif()
if(BLIT_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
if(BLIT_DEV AND BLIT_OPTIMIZE_DEBUG)
  # Exactly Release's flags, and so no -g. Do not combine -g with LTO: on
  # macOS GCC runs dsymutil after linking a -g build, and on these
  # link-time-optimized programs dsymutil (GCC 16.2, Xcode's dsymutil, Nov
  # 2026) never finished and took all the memory the machine had, hundreds
  # of GB, before it was killed. A plain Debug build is -g without LTO,
  # which is fine.
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_RELEASE}")
  if(BLIT_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG ON)
  endif()
endif()

# The GPU: Metal on Apple's systems, Vulkan elsewhere (src/gpu.h). The
# Metal side is Objective-C, which needs Apple's compiler (GCC cannot read
# Apple's framework headers); it talks to the rest through a C interface,
# so the two compilers' C++ libraries never meet. It is left out of
# link-time optimization: the two compilers' LTO objects do not mix.
# Image files are read there too, by the system's ImageIO (src/picture.h),
# SVG by AppKit, PDFs' text, links and outline by PDFKit. Elsewhere the
# Vulkan side is C++ like the rest, makes the window's surface through SDL,
# and links the Vulkan loader.
find_package(SDL3 REQUIRED CONFIG)
if(APPLE)
  if(NOT CMAKE_OBJC_COMPILER)
    set(CMAKE_OBJC_COMPILER /usr/bin/clang)
  endif()
  enable_language(OBJC)
  if(BLIT_DEV AND BLIT_OPTIMIZE_DEBUG)
    set(CMAKE_OBJC_FLAGS_DEBUG "${CMAKE_OBJC_FLAGS_RELEASE}")
  endif()
  # (Objective-C whatever C's compiler is: once C is on too, CMake would
  # give the .m files to it, and GCC, which a Homebrew build sets, cannot
  # read the SDK's headers.)
  set_source_files_properties(src/gpu_metal.m src/picture_mac.m src/macapp.m src/emoji_mac.m src/spell_mac.m src/watch_mac.m PROPERTIES LANGUAGE OBJC)
  add_library(blit_gpu STATIC src/gpu_metal.m src/picture_mac.m src/macapp.m src/spell_mac.m src/watch_mac.m)
  target_compile_options(blit_gpu PRIVATE -fobjc-arc -Wall -Wextra)
  target_link_libraries(blit_gpu PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreGraphics" "-framework ImageIO" "-framework AppKit" "-framework PDFKit" "-framework CoreServices")
  # (blit.app's notifications, src/macapp.m: a task that ended behind another window.)
  target_link_libraries(blit_gpu PUBLIC "-framework UserNotifications")
  set_target_properties(blit_gpu PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
else()
  find_package(Vulkan REQUIRED)
  add_library(blit_gpu STATIC src/gpu_vulkan.cpp src/picture_linux.cpp)
  target_link_libraries(blit_gpu PUBLIC Vulkan::Vulkan SDL3::SDL3)
  # Without link-time optimization here too, as on a Mac: GCC 16.2's got a
  # test in picture_linux.cpp wrong (Nov 2026: `w <= 0` with w = 240 came out
  # true, and an SVG asked for at 240 x 180 was drawn 1 x 1), and nothing in
  # these two files gains from it. (The build type's own setting, which a
  # Debug build compiled like a release has on, wins over the plain one.)
  set_target_properties(blit_gpu PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
  # Pictures (src/picture_linux.cpp): PNG, JPEG, GIF and WebP by their own
  # libraries, linked in (small, and nothing at startup); SVG by librsvg
  # and PDF by poppler, both drawing through cairo, which are loaded when
  # first wanted (they bring GLib and much more): only their headers are
  # needed to build, and without them blit builds without SVG or PDF.
  find_package(PNG REQUIRED)
  find_package(GIF REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(BLIT_PICTURES REQUIRED IMPORTED_TARGET libturbojpeg libwebp libwebpdemux)
  target_link_libraries(blit_gpu PUBLIC PNG::PNG GIF::GIF PkgConfig::BLIT_PICTURES ${CMAKE_DL_LIBS})
  pkg_check_modules(BLIT_POPPLER poppler-glib cairo)
  pkg_check_modules(BLIT_RSVG librsvg-2.0 cairo)
  if(BLIT_POPPLER_FOUND)
    target_include_directories(blit_gpu SYSTEM PRIVATE ${BLIT_POPPLER_INCLUDE_DIRS})
    target_compile_definitions(blit_gpu PRIVATE BLIT_HAVE_POPPLER=1)
  else()
    message(STATUS "poppler-glib's headers not found: building without PDFs")
  endif()
  if(BLIT_RSVG_FOUND)
    target_include_directories(blit_gpu SYSTEM PRIVATE ${BLIT_RSVG_INCLUDE_DIRS})
    target_compile_definitions(blit_gpu PRIVATE BLIT_HAVE_RSVG=1)
  else()
    message(STATUS "librsvg's headers not found: building without SVG")
  endif()
endif()
target_include_directories(blit_gpu PUBLIC src)

find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)

function(blit_static_runtime target)
  if(BLIT_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  if(APPLE)
    # Drops unused code, and the LTO bytecode GCC leaves in the output there.
    target_link_options(${target} PRIVATE -Wl,-dead_strip)
  endif()
endfunction()

# HarfBuzz shapes ligatures. Only its headers are needed to build: the
# library itself is loaded at run time, and only when a font asks for it, so
# it costs nothing at startup. Without the headers blit builds without
# ligature support.
find_path(HARFBUZZ_INCLUDE_DIR hb.h PATH_SUFFIXES harfbuzz)
find_library(HARFBUZZ_LIBRARY harfbuzz)

# Text storage, editing and settings: no SDL, no GPU.
add_library(blit_core STATIC src/buffer.cpp src/editor.cpp src/config.cpp src/vim.cpp src/regex.cpp src/fuzzy.cpp src/finder.cpp src/keymap.cpp src/job.cpp src/threads.cpp src/buildscan.cpp src/buildvalidation.cpp src/buildmatch.cpp src/buildtools.cpp src/buildtests.cpp src/buildprogress.cpp src/buildread.cpp)
target_include_directories(blit_core PUBLIC src)
target_link_libraries(blit_core PUBLIC Threads::Threads)
# Timelines and waveforms: lanes of changes, and dumps read into them
# (knowing no window, so that the agent can carry them later).
target_sources(blit_core PRIVATE src/timeline.cpp src/wave.cpp src/waveila.cpp)
# ELF files and address to source from their DWARF (also knowing no window).
target_sources(blit_core PRIVATE src/elf.cpp src/addrsrc.cpp src/crashscan.cpp)
# FST dumps are read through libfst (third_party/libfst, at the commit its
# config.h names), compiled as C, as src/agent/build.sh compiles it for the
# Linux agents, with zlib. Optimized in every build, and kept out of
# link-time optimization (another compiler's objects on a Mac).
enable_language(C)
if(BLIT_DEV AND BLIT_OPTIMIZE_DEBUG)
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_RELEASE}")
endif()
find_package(ZLIB REQUIRED)
add_library(blit_fst STATIC third_party/libfst/fstapi.c third_party/libfst/lz4.c third_party/libfst/fastlz.c)
target_compile_definitions(blit_fst PRIVATE FST_INCLUDE_CONFIG)
target_include_directories(blit_fst PRIVATE third_party/libfst)
target_include_directories(blit_fst SYSTEM INTERFACE third_party/libfst)
# (Clang says of its fixed-size arrays sized by a const int that they are
# an extension of GCC's; they are, and fine.)
target_compile_options(blit_fst PRIVATE -O2 $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-gnu-folding-constant>)
set_target_properties(blit_fst PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
target_link_libraries(blit_fst PUBLIC ZLIB::ZLIB)
target_link_libraries(blit_core PUBLIC blit_fst)
target_sources(blit_core PRIVATE src/wavefst.cpp)

# Working on another machine's files (src/remote.*): blit's agent runs there.
# This machine's kind of agent is built here (for any Mac of its kind, not
# just this one's processor) and carried inside blit, as are Linux's, which
# src/agent/build.sh builds (GCC 16 in a container) and which are kept with
# the sources.
if(NOT WIN32)
  add_executable(blit-agent src/agent/blit-agent.cpp src/finder.cpp src/fuzzy.cpp src/pty.cpp src/usb.cpp)
  target_include_directories(blit-agent PRIVATE src)
  target_link_libraries(blit-agent PRIVATE Threads::Threads)
  # (USB devices, for the probe picker: src/usb.cpp reads them through IOKit.)
  target_sources(blit_core PRIVATE src/usb.cpp)
  if(APPLE)
    target_link_libraries(blit-agent PRIVATE "-framework IOKit" "-framework CoreFoundation")
    target_link_libraries(blit_core PUBLIC "-framework IOKit" "-framework CoreFoundation")
  endif()
  target_compile_definitions(blit-agent PRIVATE BLIT_AGENT=1)
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    target_compile_options(blit-agent PRIVATE -mcpu=apple-m1)
  endif()
  blit_static_runtime(blit-agent)
  target_sources(blit_core PRIVATE src/remote.cpp src/profile.cpp)
  target_sources(blit_core PRIVATE src/term.cpp src/pty.cpp src/history.cpp)
  # (A terminal's pictures may come zlib compressed: kitty's graphics protocol.)
  find_package(ZLIB REQUIRED)
  target_link_libraries(blit_core PUBLIC ZLIB::ZLIB)
  add_dependencies(blit_core blit-agent)
  set_property(SOURCE src/remote.cpp APPEND PROPERTY COMPILE_DEFINITIONS BLIT_AGENT_HERE="${CMAKE_BINARY_DIR}/blit-agent")
  set_source_files_properties(src/remote.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_BINARY_DIR}/blit-agent;${CMAKE_SOURCE_DIR}/src/agent/bin/blit-agent-linux-x86_64;${CMAKE_SOURCE_DIR}/src/agent/bin/blit-agent-linux-aarch64")
  # The allocation sampler a profile of allocations puts into the program
  # (src/alloc): a shared library built here, which blit carries
  # (profile.cpp embeds it, and writes it out when it is wanted).
  enable_language(C)
  add_library(blit_alloc SHARED src/alloc/blit_alloc.c)
  set_target_properties(blit_alloc PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
  target_compile_options(blit_alloc PRIVATE -O2 -fno-omit-frame-pointer)
  target_link_libraries(blit_alloc PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  add_dependencies(blit_core blit_alloc)
  set(blit_alloc_file "${CMAKE_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}blit_alloc${CMAKE_SHARED_LIBRARY_SUFFIX}")
  set_property(SOURCE src/profile.cpp APPEND PROPERTY COMPILE_DEFINITIONS BLIT_ALLOC_HERE="${blit_alloc_file}")
  set_source_files_properties(src/profile.cpp PROPERTIES OBJECT_DEPENDS "${blit_alloc_file}")
endif()

# Glyph atlas, shaping and text -> cell grid: CPU only.
add_library(blit_view STATIC src/font.cpp src/shape.cpp src/grid.cpp)
target_link_libraries(blit_view PUBLIC blit_core Freetype::Freetype ${CMAKE_DL_LIBS})
if(APPLE)
  # Colour emoji through CoreText (emoji_mac.m), whose headers only Clang
  # reads: a library of its own, without link-time optimization, as blit_gpu.
  add_library(blit_emoji STATIC src/emoji_mac.m)
  set_target_properties(blit_emoji PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
  target_link_libraries(blit_emoji PUBLIC "-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation")
  target_link_libraries(blit_view PUBLIC blit_emoji)
endif()
# Away from Apple's systems fontconfig says which font is the system's
# monospace one, where a family is, and which fonts have a character the
# others lack. (Public: font.hpp has what it needs only where it is used.)
if(NOT APPLE)
  find_package(Fontconfig)
  if(Fontconfig_FOUND)
    target_link_libraries(blit_view PUBLIC Fontconfig::Fontconfig)
    target_compile_definitions(blit_view PUBLIC BLIT_FONTCONFIG=1)
  else()
    message(STATUS "fontconfig not found: fonts are looked for in the usual folders")
  endif()
endif()
if(HARFBUZZ_INCLUDE_DIR)
  target_include_directories(blit_view PRIVATE ${HARFBUZZ_INCLUDE_DIR})
  target_compile_definitions(blit_view PRIVATE BLIT_HAVE_HARFBUZZ=1)
  if(HARFBUZZ_LIBRARY)
    target_compile_definitions(blit_view PRIVATE BLIT_HARFBUZZ_LIBRARY="${HARFBUZZ_LIBRARY}")
  endif()
else()
  target_compile_definitions(blit_view PRIVATE BLIT_HAVE_HARFBUZZ=0)
  message(STATUS "HarfBuzz headers not found: building without ligatures")
endif()

# Syntax highlighting. tree-sitter itself is linked in (statically when the
# static library is around, which costs nothing at startup). Grammars are
# shared libraries loaded at run time; blit fetches and builds them itself
# (src/grammars.cpp).
find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h REQUIRED)
find_library(TREE_SITTER_LIBRARY NAMES libtree-sitter.a tree-sitter REQUIRED)
add_library(blit_syntax STATIC src/syntax.cpp src/match.cpp src/grammars.cpp)
target_include_directories(blit_syntax PRIVATE ${TREE_SITTER_INCLUDE_DIR})
target_link_libraries(blit_syntax PUBLIC blit_core ${TREE_SITTER_LIBRARY} ${CMAKE_DL_LIBS})

# Scripts: init.lua and plugins run in Lua 5.5, linked in (statically when
# the static library is around, as with tree-sitter). 5.5 and no other: the
# API differs from one version to the next.
find_path(LUA_INCLUDE_DIR lua.hpp PATH_SUFFIXES lua5.5 lua REQUIRED NO_CACHE)
find_library(LUA_LIBRARY NAMES liblua5.5.a liblua.a lua5.5 lua REQUIRED)
file(STRINGS "${LUA_INCLUDE_DIR}/lua.h" BLIT_LUA_MINOR REGEX "#define[ \t]+LUA_VERSION_MINOR_N")
if(NOT BLIT_LUA_MINOR MATCHES "N[ \t]+5$")
  message(FATAL_ERROR "blit needs Lua 5.5; ${LUA_INCLUDE_DIR}/lua.h is another version (brew install lua, or set LUA_INCLUDE_DIR and LUA_LIBRARY)")
endif()

# Language server client: JSON and the protocol. No SDL, no GPU.
add_library(blit_lsp STATIC src/json.cpp src/lsp.cpp)
target_link_libraries(blit_lsp PUBLIC blit_core)

# The editor. Shaders are compiled offline by shaders/build.sh and the results
# are checked in under shaders/gen, so this needs no shader tools.
add_executable(blit src/main.cpp src/app.cpp src/applsp.cpp src/apppanel.cpp src/apptabs.cpp src/appsplit.cpp src/appgit.cpp src/appvim.cpp src/appshape.cpp src/appedit.cpp src/apptest.cpp src/folds.cpp src/snippet.cpp src/appsnippet.cpp src/appfx.cpp src/appfind.cpp src/appstatus.cpp src/appstart.cpp src/appsession.cpp src/appremote.cpp src/appprofile.cpp src/appterm.cpp src/apppython.cpp src/appcommands.cpp src/appdebug.cpp src/appbuild.cpp src/appmarkdown.cpp src/appasm.cpp src/asm.cpp src/appnotebook.cpp src/notebook.cpp src/nbview.cpp src/kernel.cpp src/apppeek.cpp src/apphistory.cpp src/wrap.cpp src/appwrap.cpp src/apppr.cpp src/appfocus.cpp src/appsettings.cpp src/appupdate.cpp src/appcontainer.cpp src/watch.cpp src/appminimap.cpp src/appspell.cpp src/spell.cpp src/appimage.cpp src/apppdf.cpp src/appinstance.cpp src/instance.cpp src/loginpath.cpp src/look.cpp src/build.cpp src/engine.cpp src/tests.cpp src/fsio.cpp src/project.cpp src/libraries.cpp src/markdown.cpp src/dap.cpp src/applua.cpp src/luaapi.cpp src/git.cpp src/tree.cpp src/render.cpp src/raster.cpp src/diagram.cpp src/flowchart.cpp src/classdiagram.cpp src/sequence.cpp src/pie.cpp src/gantt.cpp src/mindmap.cpp)
# The Vulkan spec's statements of the validation layers' VUIDs.
target_sources(blit PRIVATE src/appvalidusage.cpp)
# The system's notifications (a task that ended behind another window).
target_sources(blit PRIVATE src/notify.cpp)
# The probe and port picker, and the board object on a run configuration.
target_sources(blit PRIVATE src/probes.cpp src/appprobe.cpp)
# Serial monitors: a terminal tab whose program is the device on a port,
# through termios (serial.cpp: plain POSIX, for the agent too).
target_sources(blit PRIVATE src/serial.cpp src/serialtab.cpp src/appserial.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(blit PRIVATE src/serial_linux.cpp)
endif()
# Flashing the board and programming an FPGA, and the Boards view.
target_sources(blit PRIVATE src/flash.cpp src/appflash.cpp src/boards.cpp)
# Crash dumps in a run's output, decoded against its ELF.
target_sources(blit PRIVATE src/appcrash.cpp)
# The wave tab: a dump's rows and their values (waveview.cpp), and its lanes
# in tiles and the workers that paint them (wavetile.cpp: the same pixels on
# every system, as the diagrams' below).
target_sources(blit PRIVATE src/appwave.cpp src/waveview.cpp src/wavetile.cpp)
# Simulation runs: which simulator a task runs, and the dump a run wrote,
# opened in the wave tab as it ends.
target_sources(blit PRIVATE src/simrun.cpp src/appsim.cpp)
set_source_files_properties(src/wavetile.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-lto")
# The diagrams' pictures (raster.cpp, diagram.cpp and the kinds' files) are the
# same pixels on every system: no multiply and add fused into one, which
# rounds once where two steps round twice, and which one processor does
# where another does not. And kept out of link-time optimization, which on
# x86 (GCC 16) fuses them again across the files (Nov 2026: half the
# diagrams' pixels differed from a Mac's with BLIT_LTO on, none with it
# off). They are drawn on the preview's thread, never on a frame's path.
set_source_files_properties(src/raster.cpp src/diagram.cpp src/flowchart.cpp src/classdiagram.cpp src/sequence.cpp src/pie.cpp src/gantt.cpp src/mindmap.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-lto")
target_include_directories(blit PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(blit PRIVATE blit_view blit_syntax blit_lsp blit_gpu SDL3::SDL3 ${LUA_LIBRARY})

blit_static_runtime(blit)

target_compile_definitions(blit PRIVATE BLIT_DEV=$<BOOL:${BLIT_DEV}>)
# The version blit says it is, which updates compare with the latest
# release's (the release builds pass the tag's: -DBLIT_VERSION=1.2.3).
if(NOT BLIT_VERSION)
  set(BLIT_VERSION "${PROJECT_VERSION}")
endif()
set_source_files_properties(src/appupdate.cpp PROPERTIES COMPILE_DEFINITIONS "BLIT_VERSION=\"${BLIT_VERSION}\"")
# blit.app, beside the program, in a Release build: the Dock, Spotlight,
# "Open With" for text, pictures, PDFs and folders (res/Info.plist.in), the
# icon (res/icon.py), and the license with the libraries' notices. Signed for this machine only (ad hoc). Put it in
# Applications with `cmake --install build --prefix /Applications`, or drag
# it there.
if(APPLE AND NOT BLIT_DEV)
  set(BLIT_APP ${CMAKE_BINARY_DIR}/blit.app)
  configure_file(${CMAKE_SOURCE_DIR}/res/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
  add_custom_command(TARGET blit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BLIT_APP}/Contents/MacOS ${BLIT_APP}/Contents/Resources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/Info.plist ${BLIT_APP}/Contents/Info.plist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/res/blit.icns ${BLIT_APP}/Contents/Resources/blit.icns
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/NOTICE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md ${BLIT_APP}/Contents/Resources
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:blit> ${BLIT_APP}/Contents/MacOS/blit
    COMMAND codesign --force --sign - ${BLIT_APP}
    COMMENT "blit.app"
    VERBATIM)
  install(DIRECTORY ${BLIT_APP} DESTINATION . USE_SOURCE_PERMISSIONS)
endif()
# Elsewhere, what a package puts in place: the program, its entry in the
# desktop's list of applications (res/blit.desktop), its icon, and the
# license with the libraries' notices (share/doc/blit).
if(NOT APPLE AND NOT BLIT_DEV)
  include(GNUInstallDirs)
  install(TARGETS blit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${CMAKE_SOURCE_DIR}/res/blit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(size 256 512)
    install(FILES ${CMAKE_SOURCE_DIR}/res/blit-${size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME blit.png)
  endforeach()
  install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/NOTICE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

if(BLIT_DEV)
  # The editor's benchmark and screenshot modes.
  target_sources(blit PRIVATE src/appbench.cpp)

  # Random edits checked against a plain std::string model, and unit tests.
  add_executable(blit_fuzz tests/fuzz_buffer.cpp src/tree.cpp src/git.cpp src/instance.cpp src/folds.cpp src/tests.cpp src/snippet.cpp src/asm.cpp src/notebook.cpp src/nbview.cpp src/markdown.cpp src/history.cpp src/fsio.cpp src/wrap.cpp src/dap.cpp src/look.cpp src/raster.cpp src/diagram.cpp src/flowchart.cpp src/classdiagram.cpp src/sequence.cpp src/pie.cpp src/gantt.cpp src/mindmap.cpp)
  target_link_libraries(blit_fuzz PRIVATE blit_view blit_syntax blit_lsp SDL3::SDL3 ZLIB::ZLIB)
  if(NOT WIN32)
    target_compile_definitions(blit_fuzz PRIVATE BLIT_FAKE_SSH="${CMAKE_SOURCE_DIR}/tests/fake-ssh.sh" BLIT_PROFILE_DATA="${CMAKE_SOURCE_DIR}/tests/profile")
  endif()

  # A language server that answers predictably, for the tests and benchmarks
  # (POSIX only, like the client).
  if(NOT WIN32)
    add_executable(blit_fakelsp tests/fake_lsp.cpp)
    target_link_libraries(blit_fakelsp PRIVATE blit_lsp)
    add_dependencies(blit_fuzz blit_fakelsp)
    target_compile_definitions(blit_fuzz PRIVATE BLIT_FAKE_LSP="$<TARGET_FILE:blit_fakelsp>")
    # And a debug adapter (tests/debug.sh).
    add_executable(blit_fakedap tests/fake_dap.cpp)
    target_link_libraries(blit_fakedap PRIVATE blit_lsp)
  endif()
  # The timeline model and the VCD reader, against real dumps (tests/wave.sh).
  add_executable(blit_timeline_test tests/timeline_test.cpp)
  target_link_libraries(blit_timeline_test PRIVATE blit_core)
  # The ELF reader and address to source, against addr2line (tests/elf.sh).
  add_executable(blit_elf_test tests/elf_test.cpp)
  target_link_libraries(blit_elf_test PRIVATE blit_core)

  enable_testing()
  add_test(NAME fuzz COMMAND blit_fuzz 100 600)
  if(NOT WIN32)
    # Tabs and splits through scripted mouse and key events (skipped where
    # the default font's cell size is not the one its positions assume).
    add_test(NAME splits COMMAND sh ${CMAKE_SOURCE_DIR}/tests/splits.sh $<TARGET_FILE:blit>)
    set_tests_properties(splits PROPERTIES SKIP_RETURN_CODE 77)
    # Git, end to end, in scratch repositories with a server that is a
    # folder beside them (skipped where there is no git).
    add_test(NAME git COMMAND sh ${CMAKE_SOURCE_DIR}/tests/git.sh $<TARGET_FILE:blit>)
    set_tests_properties(git PROPERTIES SKIP_RETURN_CODE 77)
    # Modal editing through the real event handlers.
    add_test(NAME modal COMMAND sh ${CMAKE_SOURCE_DIR}/tests/modal.sh $<TARGET_FILE:blit>)
    # Language servers through the editor, against the test server beside it.
    add_test(NAME lsp COMMAND sh ${CMAKE_SOURCE_DIR}/tests/lsp.sh $<TARGET_FILE:blit>)
    add_test(NAME look COMMAND sh ${CMAKE_SOURCE_DIR}/tests/look.sh $<TARGET_FILE:blit>)
    # The file tree and the disk: watched, rows chosen, trashed, dragged into folders.
    add_test(NAME tree COMMAND sh ${CMAKE_SOURCE_DIR}/tests/tree.sh $<TARGET_FILE:blit>)
    set_tests_properties(tree PROPERTIES SKIP_RETURN_CODE 77)
    # The settings screen, written into init.lua.
    add_test(NAME settings COMMAND sh ${CMAKE_SOURCE_DIR}/tests/settings.sh $<TARGET_FILE:blit>)
    # Updates, against a release made in a scratch folder (file:// URLs).
    add_test(NAME update COMMAND sh ${CMAKE_SOURCE_DIR}/tests/update.sh $<TARGET_FILE:blit>)
    set_tests_properties(update PROPERTIES SKIP_RETURN_CODE 77)
    # Dev containers, with a stand-in for docker.
    add_test(NAME container COMMAND sh ${CMAKE_SOURCE_DIR}/tests/container.sh $<TARGET_FILE:blit>)
    # The status bar's parts and clicks, a language with no colours, the window's title.
    add_test(NAME status COMMAND sh ${CMAKE_SOURCE_DIR}/tests/status.sh $<TARGET_FILE:blit>)
    # Which language a file is, what init.lua says of languages, and the ones built in.
    add_test(NAME langs COMMAND sh ${CMAKE_SOURCE_DIR}/tests/langs.sh $<TARGET_FILE:blit>)
    add_test(NAME find COMMAND sh ${CMAKE_SOURCE_DIR}/tests/find.sh $<TARGET_FILE:blit>)
    add_test(NAME shape COMMAND sh ${CMAKE_SOURCE_DIR}/tests/shape.sh $<TARGET_FILE:blit>)
    add_test(NAME edit COMMAND sh ${CMAKE_SOURCE_DIR}/tests/edit.sh $<TARGET_FILE:blit>)
    add_test(NAME testing COMMAND sh ${CMAKE_SOURCE_DIR}/tests/testing.sh $<TARGET_FILE:blit>)
    add_test(NAME lua COMMAND sh ${CMAKE_SOURCE_DIR}/tests/lua.sh $<TARGET_FILE:blit>)
    add_test(NAME start COMMAND sh ${CMAKE_SOURCE_DIR}/tests/start.sh $<TARGET_FILE:blit>)
    add_test(NAME session COMMAND sh ${CMAKE_SOURCE_DIR}/tests/session.sh $<TARGET_FILE:blit>)
    add_test(NAME build COMMAND sh ${CMAKE_SOURCE_DIR}/tests/build.sh $<TARGET_FILE:blit>)
    # A folder's own tasks (.blit/tasks.json): variables, long tasks, Stop, the Lua events, notifications.
    add_test(NAME tasks COMMAND sh ${CMAKE_SOURCE_DIR}/tests/tasks.sh $<TARGET_FILE:blit>)
    # Vulkan's and Metal's validation messages as problems, and the spec's statements of VUIDs.
    add_test(NAME validation COMMAND sh ${CMAKE_SOURCE_DIR}/tests/validation.sh $<TARGET_FILE:blit>)
    add_test(NAME buildtools COMMAND sh ${CMAKE_SOURCE_DIR}/tests/buildtools.sh $<TARGET_FILE:blit>)
    # How far a flash or an FPGA's load has got, from the engines' output replayed (tests/replay.sh).
    add_test(NAME progress COMMAND sh ${CMAKE_SOURCE_DIR}/tests/progress.sh $<TARGET_FILE:blit>)
    # Engines run with no terminal tab, here and there (tests/fake-ssh.sh): lines, JSON lines, a time limit, a cancel, which.
    add_test(NAME engine COMMAND sh ${CMAKE_SOURCE_DIR}/tests/engine.sh $<TARGET_FILE:blit>)
    add_test(NAME problems COMMAND sh ${CMAKE_SOURCE_DIR}/tests/problems.sh $<TARGET_FILE:blit>)
    add_test(NAME markdown COMMAND sh ${CMAKE_SOURCE_DIR}/tests/markdown.sh $<TARGET_FILE:blit>)
    # A notebook's view beside its script (skipped with no python3 to write the notebooks).
    add_test(NAME notebook COMMAND sh ${CMAKE_SOURCE_DIR}/tests/notebook.sh $<TARGET_FILE:blit>)
    set_tests_properties(notebook PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME images COMMAND sh ${CMAKE_SOURCE_DIR}/tests/images.sh $<TARGET_FILE:blit>)
    add_test(NAME remote COMMAND sh ${CMAKE_SOURCE_DIR}/tests/remote.sh $<TARGET_FILE:blit>)
    add_test(NAME agent COMMAND sh ${CMAKE_SOURCE_DIR}/tests/agent.sh $<TARGET_FILE:blit>)
    add_test(NAME profile COMMAND sh ${CMAKE_SOURCE_DIR}/tests/profile.sh $<TARGET_FILE:blit>)
    add_test(NAME profviews COMMAND sh ${CMAKE_SOURCE_DIR}/tests/profviews.sh $<TARGET_FILE:blit>)
    add_test(NAME term COMMAND sh ${CMAKE_SOURCE_DIR}/tests/term.sh $<TARGET_FILE:blit>)
    # (Recording a profile, watching programs in a terminal and taking in a
    # notebook's 20 MB of output go by time: run alone, they are not slowed
    # by another suite on a busy machine.)
    set_tests_properties(profile term notebook PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME keys COMMAND sh ${CMAKE_SOURCE_DIR}/tests/keys.sh $<TARGET_FILE:blit>)
    add_test(NAME debug COMMAND sh ${CMAKE_SOURCE_DIR}/tests/debug.sh $<TARGET_FILE:blit>)
    # The probe and port picker: USB devices named, the board object written, here and on another machine.
    add_test(NAME probes COMMAND sh ${CMAKE_SOURCE_DIR}/tests/probes.sh $<TARGET_FILE:blit>)
    # Flashing: each tool's command line, its progress, only if changed, the chord, the Boards view, here and on another machine.
    add_test(NAME flash COMMAND sh ${CMAKE_SOURCE_DIR}/tests/flash.sh $<TARGET_FILE:blit>)
    # Serial monitor tabs, on a pseudo-terminal standing in for a board: what comes and goes, times, hex, a reset's gap, flashing, the picker.
    add_test(NAME serial COMMAND sh ${CMAKE_SOURCE_DIR}/tests/serial.sh $<TARGET_FILE:blit>)
    # The project's Python: found for each project and subproject, and what takes it.
    add_test(NAME python COMMAND sh ${CMAKE_SOURCE_DIR}/tests/python.sh $<TARGET_FILE:blit>)
    # Characters of more than one code point (marks, emoji sequences, flags) and of other widths.
    add_test(NAME unicode COMMAND sh ${CMAKE_SOURCE_DIR}/tests/unicode.sh $<TARGET_FILE:blit>)
    # Waveforms: the timeline model and the VCD reader, on dumps from Icarus, Verilator, Yosys and pyvcd.
    add_test(NAME wave COMMAND sh ${CMAKE_SOURCE_DIR}/tests/wave.sh $<TARGET_FILE:blit>)
    # The wave tab: a dump's waves, its keys, radixes, markers, groups, the view kept, its tiles' pixels.
    add_test(NAME wavetab COMMAND sh ${CMAKE_SOURCE_DIR}/tests/wavetab.sh $<TARGET_FILE:blit>)
    # Simulation runs: Verilator, Icarus and SymbiYosys tasks opening the dump they wrote; the last run's waves on gW.
    add_test(NAME simrun COMMAND sh ${CMAKE_SOURCE_DIR}/tests/simrun.sh $<TARGET_FILE:blit>)
    # The ELF reader and address to source against addr2line (tests/elf.sh),
    # and crash dumps in a run's output decoded in place (tests/crash.sh).
    add_test(NAME elf COMMAND sh ${CMAKE_SOURCE_DIR}/tests/elf.sh $<TARGET_FILE:blit>)
    add_test(NAME crash COMMAND sh ${CMAKE_SOURCE_DIR}/tests/crash.sh $<TARGET_FILE:blit>)
    # Every suite sees the dark appearance, whatever the machine is set to
    # (GitHub's machines are light), and the cell macOS's own font has at
    # 13 pt (16 x 31 pixels at twice the density), whatever fonts the
    # machine has: the cases' mouse positions, rows and corners are for it.
    get_property(blit_tests DIRECTORY PROPERTY TESTS)
    set(blit_test_env BLIT_APPEARANCE=dark)
    if(NOT APPLE)
      list(APPEND blit_test_env BLIT_CELL=16x31)
    endif()
    set_tests_properties(${blit_tests} PROPERTIES ENVIRONMENT "${blit_test_env}")
  endif()

  # Benchmark tools (POSIX only). See bench/run.sh.
  if(NOT WIN32)
    add_executable(blit_bench bench/bench.cpp src/git.cpp)
    target_link_libraries(blit_bench PRIVATE blit_view blit_syntax blit_lsp)
    add_dependencies(blit_bench blit_fakelsp)
    target_compile_definitions(blit_bench PRIVATE BLIT_FAKE_LSP="$<TARGET_FILE:blit_fakelsp>")

    # Reference point for startup time: an SDL_GPU program that draws nothing.
    add_executable(blit_null bench/null_app.cpp)
    target_link_libraries(blit_null PRIVATE SDL3::SDL3)
    blit_static_runtime(blit_null)
  endif()
endif()
