cmake_minimum_required(VERSION 3.19)

project(lxlyrics VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# FooyinConfig.cmake provides Fooyin::Core / Fooyin::Gui / Fooyin::Utils and the
# create_fooyin_plugin() macro (see /usr/lib/cmake/fooyin/FooyinMacros.cmake).
find_package(Fooyin REQUIRED)

# FooyinConfig does NOT propagate Qt modules (it only finds Core/Widgets/Sql/
# Concurrent/Network), so declare the plugin's own components here explicitly.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets)

# Macro signature (Fooyin 0.11.1): create_fooyin_plugin(name [NO_INSTALL]
# [JSON_IN <file>] [DEPENDS <libs...>] [SOURCES <files...>]).
# It builds a MODULE library named fyplugin_lxlyrics, configures lxlyrics.json.in
# -> lxlyrics.json in the build dir (the file referenced by Q_PLUGIN_METADATA),
# enables AUTOMOC, and installs to FOOYIN_PLUGIN_INSTALL_DIR.
# feedwriter.cpp must stay free of Fooyin headers: the app's feed test suite
# compiles it as the host side of the wire contract (the same cross-project
# pattern the pre-v2 protocol suite used).
create_fooyin_plugin(lxlyrics
    DEPENDS Fooyin::Core Fooyin::Gui Qt6::Widgets
    SOURCES
        src/lxlyricsplugin.cpp
        src/lxlyricsplugin.h
        src/feedwriter.cpp
        src/feedwriter.h
        src/playerbridge.cpp
        src/playerbridge.h
        src/spectrumsource.cpp
        src/spectrumsource.h
        src/lxlyricssettings.cpp
        src/lxlyricssettings.h
)
