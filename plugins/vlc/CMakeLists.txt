cmake_minimum_required(VERSION 3.16)
project(lx-lyrics-vlc-plugin C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Support Debug and Release single-config generators, matching the app project:
# a bare `cmake -B build -G Ninja` must not produce an unoptimised plugin.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# VLC module headers. `pkg-config vlc-plugin` is the canonical source of the
# flags a VLC plugin is built with: VLC installs its module headers into
# <includedir>/vlc/plugins (upstream src/Makefile.am:22-24, `pluginsincludedir =
# $(pkgincludedir)/plugins`) and its .pc exports
#   Cflags: -I${pkgincludedir}/plugins -D__PLUGIN__ -D_FILE_OFFSET_BITS=64 ...
#   Libs:   -lvlccore
# A source checkout works too: pass -DVLC_INCLUDE_DIR=<vlc>/include and
# -DVLC_LIBRARY=<vlc>/src/.libs/libvlccore.so.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(VLC_PLUGIN vlc-plugin)
endif()

set(VLC_INCLUDE_DIR "" CACHE PATH "VLC module include dir (holds vlc_plugin.h)")
if(NOT VLC_INCLUDE_DIR AND VLC_PLUGIN_INCLUDE_DIRS)
  set(VLC_INCLUDE_DIR "${VLC_PLUGIN_INCLUDE_DIRS}")
endif()
if(NOT VLC_INCLUDE_DIR)
  find_path(VLC_INCLUDE_DIR vlc_plugin.h PATH_SUFFIXES vlc/plugins vlc)
endif()
if(NOT VLC_INCLUDE_DIR)
  message(FATAL_ERROR
    "VLC module headers (vlc_plugin.h) not found. Install VLC's development files "
    "(they ship a `vlc-plugin` pkg-config module) or pass "
    "-DVLC_INCLUDE_DIR=<vlc-src>/include from "
    "`git clone --depth 1 --branch 3.0.23 https://github.com/videolan/vlc`.")
endif()

set(VLC_LIBRARY "" CACHE FILEPATH "Path to libvlccore (optional; pkg-config supplies it)")
if(NOT VLC_LIBRARY AND NOT VLC_PLUGIN_LIBRARIES)
  find_library(VLC_LIBRARY NAMES vlccore)
endif()

find_package(Threads REQUIRED)

# The file name IS the module name: VLC only loads plugins matching
# `lib*_plugin.so` (src/modules/bank.c:418-427) and derives the module's name —
# the string `--extraintf` and the preferences list match against — from that file
# name (modules/common.am:18-27 strips "lib" and "_plugin*" and defines
# -DMODULE_STRING=... from the result). Hence: liblxlyrics_plugin.so + the
# MODULE_STRING definition below, both pinned to "lxlyrics".
add_library(lxlyrics_plugin MODULE
  src/plugin.c
  src/feed.c
  src/json.c
  src/mapper.c
)
set_target_properties(lxlyrics_plugin PROPERTIES
  OUTPUT_NAME "lxlyrics_plugin"
  # VLC 3.0's headers require C11 (its configure selects a C11 compiler —
  # upstream configure.ac:48 `VLC_PROG_CC_C11`, m4/c11.m4:12-29 — and
  # vlc_common.h:480 is built on _Generic). The hermetic sources below stay
  # strict C99; only this target sees VLC's headers.
  C_STANDARD 11
  # Upstream restricts the exported symbols to the entry points
  # (`-export-symbols-regex ^vlc_entry`, modules/common.am:37-43). __PLUGIN__
  # marks the entry function default-visibility (include/vlc_plugin.h:230-232),
  # so hiding everything else leaves exactly vlc_entry__3_0_0f exported.
  C_VISIBILITY_PRESET hidden
)
target_compile_definitions(lxlyrics_plugin PRIVATE
  MODULE_STRING=\"lxlyrics\"
)
target_include_directories(lxlyrics_plugin PRIVATE "${VLC_INCLUDE_DIR}" src)
target_compile_options(lxlyrics_plugin PRIVATE -Wall -Wextra ${VLC_PLUGIN_CFLAGS_OTHER})
if(VLC_PLUGIN_LIBRARIES)
  target_link_libraries(lxlyrics_plugin PRIVATE ${VLC_PLUGIN_LINK_LIBRARIES})
else()
  target_link_libraries(lxlyrics_plugin PRIVATE "${VLC_LIBRARY}")
endif()

# Hermetic wire tests: feed.c + json.c against a stub child, and the mapping
# logic on its own. No VLC headers, player or display server needed.
enable_testing()
add_executable(feed_test tests/feed_test.c src/feed.c src/json.c)
target_include_directories(feed_test PRIVATE src)
target_compile_options(feed_test PRIVATE -Wall -Wextra)
target_link_libraries(feed_test PRIVATE Threads::Threads)
add_test(NAME feed_test COMMAND feed_test)

add_executable(mapper_test tests/mapper_test.c src/mapper.c)
target_include_directories(mapper_test PRIVATE src)
target_compile_options(mapper_test PRIVATE -Wall -Wextra)
add_test(NAME mapper_test COMMAND mapper_test)
