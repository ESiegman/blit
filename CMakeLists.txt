cmake_minimum_required(VERSION 3.28)
project(blit VERSION 0.1.0 LANGUAGES CXX)

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
# SVG by AppKit, PDFs' text, links and outline by PDFKit.
if(APPLE)
  if(NOT CMAKE_OBJC_COMPILER)
    set(CMAKE_OBJC_COMPILER /usr/bin/clang)
  endif()
  enable_language(OBJC)
  if(BLIT_DEV AND BLIT_OPTIMIZE_DEBUG)
    set(CMAKE_OBJC_FLAGS_DEBUG "${CMAKE_OBJC_FLAGS_RELEASE}")
  endif()
  add_library(blit_gpu STATIC src/gpu_metal.m src/picture_mac.m src/macapp.m)
  target_compile_options(blit_gpu PRIVATE -fobjc-arc -Wall -Wextra)
  target_link_libraries(blit_gpu PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreGraphics" "-framework ImageIO" "-framework AppKit" "-framework PDFKit")
else()
  add_library(blit_gpu STATIC src/gpu_vulkan.cpp)
endif()
set_target_properties(blit_gpu PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
target_include_directories(blit_gpu PUBLIC src)

find_package(SDL3 REQUIRED CONFIG)
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
add_library(blit_core STATIC src/buffer.cpp src/editor.cpp src/config.cpp src/vim.cpp src/regex.cpp src/fuzzy.cpp src/finder.cpp src/keymap.cpp src/job.cpp src/threads.cpp src/buildscan.cpp)
target_include_directories(blit_core PUBLIC src)
target_link_libraries(blit_core PUBLIC Threads::Threads)

# Working on another machine's files (src/remote.*): blit's agent runs there.
# This machine's kind of agent is built here (for any Mac of its kind, not
# just this one's processor) and carried inside blit, as are Linux's, which
# src/agent/build.sh builds (GCC 16 in a container) and which are kept with
# the sources.
if(NOT WIN32)
  add_executable(blit-agent src/agent/blit-agent.cpp src/finder.cpp src/fuzzy.cpp src/pty.cpp)
  target_include_directories(blit-agent PRIVATE src)
  target_link_libraries(blit-agent PRIVATE Threads::Threads)
  target_compile_definitions(blit-agent PRIVATE BLIT_AGENT=1)
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    target_compile_options(blit-agent PRIVATE -mcpu=apple-m1)
  endif()
  blit_static_runtime(blit-agent)
  target_sources(blit_core PRIVATE src/remote.cpp src/profile.cpp)
  target_sources(blit_core PRIVATE src/term.cpp src/pty.cpp)
  # (A terminal's pictures may come zlib compressed: kitty's graphics protocol.)
  find_package(ZLIB REQUIRED)
  target_link_libraries(blit_core PUBLIC ZLIB::ZLIB)
  add_dependencies(blit_core blit-agent)
  set_property(SOURCE src/remote.cpp APPEND PROPERTY COMPILE_DEFINITIONS BLIT_AGENT_HERE="${CMAKE_BINARY_DIR}/blit-agent")
  set_source_files_properties(src/remote.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_BINARY_DIR}/blit-agent;${CMAKE_SOURCE_DIR}/src/agent/bin/blit-agent-linux-x86_64;${CMAKE_SOURCE_DIR}/src/agent/bin/blit-agent-linux-aarch64")
endif()

# Glyph atlas, shaping and text -> cell grid: CPU only.
add_library(blit_view STATIC src/font.cpp src/shape.cpp src/grid.cpp)
target_link_libraries(blit_view PUBLIC blit_core Freetype::Freetype ${CMAKE_DL_LIBS})
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
add_executable(blit src/main.cpp src/app.cpp src/applsp.cpp src/apppanel.cpp src/apptabs.cpp src/appsplit.cpp src/appgit.cpp src/appvim.cpp src/appfx.cpp src/appfind.cpp src/appstatus.cpp src/appstart.cpp src/appsession.cpp src/appremote.cpp src/appprofile.cpp src/appterm.cpp src/appcommands.cpp src/appdebug.cpp src/appbuild.cpp src/appmarkdown.cpp src/appimage.cpp src/apppdf.cpp src/appinstance.cpp src/instance.cpp src/loginpath.cpp src/look.cpp src/build.cpp src/fsio.cpp src/project.cpp src/libraries.cpp src/markdown.cpp src/mermaid.cpp src/dap.cpp src/applua.cpp src/luaapi.cpp src/git.cpp src/tree.cpp src/render.cpp)
target_include_directories(blit PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(blit PRIVATE blit_view blit_syntax blit_lsp blit_gpu SDL3::SDL3 ${LUA_LIBRARY})

blit_static_runtime(blit)

target_compile_definitions(blit PRIVATE BLIT_DEV=$<BOOL:${BLIT_DEV}>)
# blit.app, beside the program, in a Release build: the Dock, Spotlight,
# "Open With" for text, pictures, PDFs and folders (res/Info.plist.in), and
# the icon (res/icon.py). Signed for this machine only (ad hoc). Put it in
# Applications with `cmake --install build --prefix /Applications`, or drag
# it there.
if(APPLE AND NOT BLIT_DEV)
  set(BLIT_APP ${CMAKE_BINARY_DIR}/blit.app)
  configure_file(${CMAKE_SOURCE_DIR}/res/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
  add_custom_command(TARGET blit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BLIT_APP}/Contents/MacOS ${BLIT_APP}/Contents/Resources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/Info.plist ${BLIT_APP}/Contents/Info.plist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/res/blit.icns ${BLIT_APP}/Contents/Resources/blit.icns
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:blit> ${BLIT_APP}/Contents/MacOS/blit
    COMMAND codesign --force --sign - ${BLIT_APP}
    COMMENT "blit.app"
    VERBATIM)
  install(DIRECTORY ${BLIT_APP} DESTINATION . USE_SOURCE_PERMISSIONS)
endif()

if(BLIT_DEV)
  # The editor's benchmark and screenshot modes.
  target_sources(blit PRIVATE src/appbench.cpp)

  # Random edits checked against a plain std::string model, and unit tests.
  add_executable(blit_fuzz tests/fuzz_buffer.cpp src/tree.cpp src/git.cpp src/instance.cpp)
  target_link_libraries(blit_fuzz PRIVATE blit_view blit_syntax blit_lsp SDL3::SDL3)
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
  endif()
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
    # The status bar's parts and clicks, a language with no colours, the window's title.
    add_test(NAME status COMMAND sh ${CMAKE_SOURCE_DIR}/tests/status.sh $<TARGET_FILE:blit>)
    add_test(NAME find COMMAND sh ${CMAKE_SOURCE_DIR}/tests/find.sh $<TARGET_FILE:blit>)
    add_test(NAME lua COMMAND sh ${CMAKE_SOURCE_DIR}/tests/lua.sh $<TARGET_FILE:blit>)
    add_test(NAME start COMMAND sh ${CMAKE_SOURCE_DIR}/tests/start.sh $<TARGET_FILE:blit>)
    add_test(NAME session COMMAND sh ${CMAKE_SOURCE_DIR}/tests/session.sh $<TARGET_FILE:blit>)
    add_test(NAME build COMMAND sh ${CMAKE_SOURCE_DIR}/tests/build.sh $<TARGET_FILE:blit>)
    add_test(NAME markdown COMMAND sh ${CMAKE_SOURCE_DIR}/tests/markdown.sh $<TARGET_FILE:blit>)
    add_test(NAME images COMMAND sh ${CMAKE_SOURCE_DIR}/tests/images.sh $<TARGET_FILE:blit>)
    add_test(NAME remote COMMAND sh ${CMAKE_SOURCE_DIR}/tests/remote.sh $<TARGET_FILE:blit>)
    add_test(NAME profile COMMAND sh ${CMAKE_SOURCE_DIR}/tests/profile.sh $<TARGET_FILE:blit>)
    add_test(NAME term COMMAND sh ${CMAKE_SOURCE_DIR}/tests/term.sh $<TARGET_FILE:blit>)
    add_test(NAME keys COMMAND sh ${CMAKE_SOURCE_DIR}/tests/keys.sh $<TARGET_FILE:blit>)
    add_test(NAME debug COMMAND sh ${CMAKE_SOURCE_DIR}/tests/debug.sh $<TARGET_FILE:blit>)
    # Characters of more than one code point (marks, emoji sequences, flags) and of other widths.
    add_test(NAME unicode COMMAND sh ${CMAKE_SOURCE_DIR}/tests/unicode.sh $<TARGET_FILE:blit>)
    # Every suite sees the dark appearance, whatever the machine is set to
    # (GitHub's machines are light).
    get_property(blit_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${blit_tests} PROPERTIES ENVIRONMENT BLIT_APPEARANCE=dark)
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
