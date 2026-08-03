cmake_minimum_required(VERSION 3.21)
project(lyrics-app VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Support Debug and Release single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS
    Widgets
    Network
    WebSockets
)

add_executable(lyrics-app
    src/main.cpp
    src/app/appcontext.h
    src/app/clioptions.h
    src/app/lyriccontroller.cpp
    src/app/lyriccontroller.h
    src/app/spectrumbridge.cpp
    src/app/spectrumbridge.h
    src/bridge/pausehide.cpp
    src/bridge/pausehide.h
    src/bridge/wsclient.cpp
    src/bridge/wsclient.h
    src/config/desktoplyricconfig.cpp
    src/config/desktoplyricconfig.h
    src/engine/lrcparser.cpp
    src/engine/lrcparser.h
    src/engine/wordparser.cpp
    src/engine/wordparser.h
    src/engine/lyricplayer.cpp
    src/engine/lyricplayer.h
    src/engine/lyricclock.cpp
    src/engine/lyricclock.h
    src/engine/lyricselector.cpp
    src/engine/lyricselector.h
    src/i18n/translationmanager.cpp
    src/i18n/translationmanager.h
    src/renderer/controlbar.cpp
    src/renderer/controlbar.h
    src/renderer/lyricrenderer.cpp
    src/renderer/lyricrenderer.h
    src/renderer/spectrumwidget.cpp
    src/renderer/spectrumwidget.h
    src/settings/colorpickerbutton.cpp
    src/settings/colorpickerbutton.h
    src/settings/settingsdialog.cpp
    src/settings/settingsdialog.h
    src/window/lyricwindow.cpp
    src/window/lyricwindow.h
    translations.qrc
)

target_include_directories(lyrics-app PRIVATE src/)

target_link_libraries(lyrics-app PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
)

# Tests for the pure lyric parsing engine.
enable_testing()
find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

add_executable(lyrics-app-tests
    tests/tst_engine.cpp
    src/engine/lrcparser.cpp
    src/engine/wordparser.cpp
    src/engine/lyricselector.cpp
    src/engine/lyricselector.h
)

target_include_directories(lyrics-app-tests PRIVATE src/)

target_link_libraries(lyrics-app-tests PRIVATE
    Qt6::Test
)

add_test(NAME engine COMMAND lyrics-app-tests)

# Tests for the LyricPlayer timed lyric player engine.
add_executable(lyrics-app-lyricplayer-tests
    tests/tst_lyricplayer.cpp
    src/engine/lrcparser.cpp
    src/engine/wordparser.cpp
    src/engine/lyricclock.cpp
    src/engine/lyricplayer.cpp
)

target_include_directories(lyrics-app-lyricplayer-tests PRIVATE src/)

target_link_libraries(lyrics-app-lyricplayer-tests PRIVATE
    Qt6::Test
)

add_test(NAME lyricplayer COMMAND lyrics-app-lyricplayer-tests)

# Tests for the WebSocket bridge protocol round-trip (docs/protocol.md).
add_executable(lyrics-app-protocol-tests
    tests/tst_protocol.cpp
    src/bridge/wsclient.cpp
    src/bridge/wsclient.h
)

target_include_directories(lyrics-app-protocol-tests PRIVATE src/)

target_link_libraries(lyrics-app-protocol-tests PRIVATE
    Qt6::Test
    Qt6::WebSockets
)

add_test(NAME protocol COMMAND lyrics-app-protocol-tests)

# Tests for the DesktopLyricConfig persistence store.
add_executable(lyrics-app-config-tests
    tests/tst_config.cpp
    src/config/desktoplyricconfig.cpp
    src/config/desktoplyricconfig.h
)

target_include_directories(lyrics-app-config-tests PRIVATE src/)

target_link_libraries(lyrics-app-config-tests PRIVATE
    Qt6::Test
    Qt6::Gui
)

add_test(NAME config COMMAND lyrics-app-config-tests)
