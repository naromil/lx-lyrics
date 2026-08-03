cmake_minimum_required(VERSION 3.19)

project(lxlyrics VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# FooyinConfig.cmake provides Fooyin::Core / Fooyin::Gui / Fooyin::Utils and the
# create_fooyin_plugin() macro (see /usr/lib/cmake/fooyin/FooyinMacros.cmake).
find_package(Fooyin REQUIRED)

# FooyinConfig does NOT propagate WebSockets (it only finds Core/Widgets/Sql/
# Concurrent/Network), so declare it here explicitly. Later tasks (the local
# WebSocket server) need it; harmless for the scaffold.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets WebSockets)

# Fooyin::Core's interface link libraries (see FooyinTargets.cmake) carry
# Qt6 + Taglib but NOT ICU, so declare it here explicitly. lyricsources.cpp
# converts non-UTF-8 lyric bytes via ICU (GB18030/BIG5); the bundled
# FindICU.cmake module provides the ICU::uc imported target.
find_package(ICU REQUIRED COMPONENTS uc)

# Macro signature (Fooyin 0.11.1): create_fooyin_plugin(name [NO_INSTALL]
# [JSON_IN <file>] [DEPENDS <libs...>] [SOURCES <files...>]).
# It builds a MODULE library named fyplugin_lxlyrics, configures lxlyrics.json.in
# -> lxlyrics.json in the build dir (the file referenced by Q_PLUGIN_METADATA),
# enables AUTOMOC, and installs to FOOYIN_PLUGIN_INSTALL_DIR.
create_fooyin_plugin(lxlyrics
    DEPENDS Fooyin::Core Fooyin::Gui Qt6::Widgets Qt6::WebSockets ICU::uc
    SOURCES
        src/lxlyricsplugin.cpp
        src/lxlyricsplugin.h
        src/hostserver.cpp
        src/hostserver.h
        src/playerbridge.cpp
        src/playerbridge.h
        src/spectrumsource.cpp
        src/spectrumsource.h
        src/appspawner.cpp
        src/appspawner.h
        src/lxlyricssettings.cpp
        src/lxlyricssettings.h
        src/lyricsources.cpp
        src/lyricsources.h
)
