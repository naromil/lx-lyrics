cmake_minimum_required(VERSION 3.16)
project(lx-lyrics-deadbeef-plugin C)

# C11 for src/hotkey.c's <stdatomic.h> reader-stop flag (C99 is a subset).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Support Debug and Release single-config generators, matching the app project:
# a bare `cmake -B build -G Ninja` must not produce an unoptimised plugin.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# DeaDBeeF headers: a source checkout's include/ directory, or an installed SDK
# (the file to find is deadbeef/deadbeef.h).
#   cmake -B build -G Ninja -DDEADBEEF_INCLUDE_DIR=/path/to/deadbeef/include
set(DEADBEEF_INCLUDE_DIR "" CACHE PATH "DeaDBeeF include dir (holds deadbeef/deadbeef.h)")
if(NOT DEADBEEF_INCLUDE_DIR)
  find_path(DEADBEEF_INCLUDE_DIR deadbeef/deadbeef.h)
endif()
if(NOT DEADBEEF_INCLUDE_DIR)
  message(FATAL_ERROR
    "DeaDBeeF headers not found. Pass -DDEADBEEF_INCLUDE_DIR=<deadbeef>/include, e.g. from "
    "`git clone --depth 1 --branch 1.10.1 https://github.com/DeaDBeeF-Player/deadbeef`.")
endif()

find_package(Threads REQUIRED)

# The module must be ddb_lxlyrics.so: DeaDBeeF derives the entry symbol from the
# file basename (ddb_lxlyrics.so -> ddb_lxlyrics_load), so no "lib" prefix.
add_library(ddb_lxlyrics MODULE
  src/plugin.c
  src/feed.c
  src/hotkey.c
  src/json.c
  src/spectrum.c
)
set_target_properties(ddb_lxlyrics PROPERTIES PREFIX "" OUTPUT_NAME "ddb_lxlyrics")
target_include_directories(ddb_lxlyrics PRIVATE "${DEADBEEF_INCLUDE_DIR}" src)
target_compile_options(ddb_lxlyrics PRIVATE -Wall -Wextra)
target_link_libraries(ddb_lxlyrics PRIVATE Threads::Threads m)

# Hermetic wire tests: feed.c + json.c against a stub child, no DeaDBeeF headers.
enable_testing()
add_executable(feed_test tests/feed_test.c src/feed.c src/json.c)
target_include_directories(feed_test PRIVATE src)
target_compile_options(feed_test PRIVATE -Wall -Wextra)
target_link_libraries(feed_test PRIVATE Threads::Threads)
add_test(NAME feed_test COMMAND feed_test)

# Hermetic global-shortcut transport tests: hotkey.c only, with /bin/sh
# stand-ins speaking the helper protocol on fd 3 (no DeaDBeeF headers, no
# player, no display server).
add_executable(hotkey_test tests/hotkey_test.c src/hotkey.c)
target_include_directories(hotkey_test PRIVATE src)
target_compile_options(hotkey_test PRIVATE -Wall -Wextra)
target_link_libraries(hotkey_test PRIVATE Threads::Threads)
add_test(NAME hotkey_test COMMAND hotkey_test)
