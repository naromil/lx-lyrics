cmake_minimum_required(VERSION 3.16)
project(lx-lyrics-audacious-plugin C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# libaudcore's headers are C++17 (upstream builds with cpp_std=gnu++17, meson.build:6).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Support Debug and Release single-config generators, matching the app project:
# a bare `cmake -B build -G Ninja` must not produce an unoptimised plugin.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# Audacious headers (the file to find is libaudcore/plugin.h) and the libaudcore
# link line, discovered through pkg-config or given explicitly:
#   cmake -B build -G Ninja -DAUDACIOUS_INCLUDE_DIR=/path/to/include
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(AUDACIOUS_PC QUIET audacious)
endif()

set(AUDACIOUS_INCLUDE_DIR "${AUDACIOUS_PC_INCLUDE_DIRS}" CACHE PATH
    "Audacious include dir (holds libaudcore/plugin.h)")
if(NOT AUDACIOUS_INCLUDE_DIR)
  find_path(AUDACIOUS_INCLUDE_DIR libaudcore/plugin.h)
endif()
if(NOT AUDACIOUS_INCLUDE_DIR)
  message(FATAL_ERROR
    "Audacious headers not found. Install Audacious >= 4.6.1 with its headers (the file to "
    "find is libaudcore/plugin.h), or pass -DAUDACIOUS_INCLUDE_DIR=<prefix>/include. "
    "`pkg-config --variable=audacious_include_dir audacious` names the installed one; a "
    "source checkout's headers sit in <audacious>/src/libaudcore once it is built.")
endif()

set(AUDACIOUS_LIBRARIES "${AUDACIOUS_PC_LIBRARIES}" CACHE STRING "libaudcore link name(s)")
if(NOT AUDACIOUS_LIBRARIES)
  set(AUDACIOUS_LIBRARIES audcore)
endif()

find_package(Threads REQUIRED)

# The module must be lxlyrics.so: the loader scans <PluginDir>/General/*.so, takes
# the basename as the plugin id and looks up aud_plugin_instance inside
# (plugin-load.cc:33-34, :75-91), and upstream builds its plugins with
# name_prefix: '' (audacious-plugins shared_module calls).
add_library(lxlyrics MODULE
  src/plugin.cc
  src/feed.c
  src/json.c
  src/spectrum.cc
)
set_target_properties(lxlyrics PROPERTIES PREFIX "" OUTPUT_NAME "lxlyrics")
target_include_directories(lxlyrics PRIVATE "${AUDACIOUS_INCLUDE_DIR}" src)
target_compile_options(lxlyrics PRIVATE -Wall -Wextra)
target_link_libraries(lxlyrics PRIVATE ${AUDACIOUS_LIBRARIES} Threads::Threads m)

# Hermetic wire tests: feed.c + json.c against a stub child, no Audacious headers,
# no player, no display server.
enable_testing()
add_executable(feed_test tests/feed_test.c src/feed.c src/json.c)
target_include_directories(feed_test PRIVATE src)
target_compile_options(feed_test PRIVATE -Wall -Wextra)
target_link_libraries(feed_test PRIVATE Threads::Threads)
add_test(NAME feed_test COMMAND feed_test)
