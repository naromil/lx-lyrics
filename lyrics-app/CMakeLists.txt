cmake_minimum_required(VERSION 3.21)
project(lx-lyrics-app VERSION 0.1.0 LANGUAGES CXX)

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

# Widgets only: the feed is stdin/stdout pipes to the parent process, so the
# WebSocket transport (and Qt Network with it) is gone.
find_package(Qt6 6.4 REQUIRED COMPONENTS
    Widgets
)

# Lyric acquisition happens in the app now (the host sends the file path), so
# the app decodes legacy encodings with ICU and reads embedded tags with TagLib.
find_package(ICU REQUIRED COMPONENTS uc)

# TagLib: prefer the CONFIG package (target TagLib::tag), fall back to
# pkg-config (target PkgConfig::TagLib) on installs that ship no CMake files.
find_package(TagLib CONFIG QUIET)
if(NOT TARGET TagLib::tag)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TagLib REQUIRED IMPORTED_TARGET taglib)
    set(LX_TAGLIB_TARGET PkgConfig::TagLib)
else()
    set(LX_TAGLIB_TARGET TagLib::tag)
endif()

add_executable(lx-lyrics-app
    src/main.cpp
    src/app/appcontext.h
    src/app/clioptions.h
    src/app/lyriccontroller.cpp
    src/app/lyriccontroller.h
    src/app/spectrumbridge.cpp
    src/app/spectrumbridge.h
    src/app/spectrumtransport.cpp
    src/app/spectrumtransport.h
    src/bridge/pausehide.cpp
    src/bridge/pausehide.h
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
    src/host/feedreader.cpp
    src/host/feedreader.h
    src/host/feedtypes.h
    src/host/pipeio.cpp
    src/host/pipeio.h
    src/host/tracklyrics.cpp
    src/host/tracklyrics.h
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

target_include_directories(lx-lyrics-app PRIVATE src/)

target_link_libraries(lx-lyrics-app PRIVATE
    Qt6::Widgets
    ICU::uc
    ${LX_TAGLIB_TARGET}
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

# Tests for the player feed (docs/protocol.md v2): the app's FeedReader and the
# plugin's FeedWriter are BOTH compiled in — they are the two halves of the
# wire contract, and FeedWriter is Fooyin-header-free precisely so it can be
# (the same cross-project pattern the deleted protocol suite used). pipeio.cpp
# is here so the EOF path runs against a real pipe.
add_executable(lyrics-app-feed-tests
    tests/tst_feed.cpp
    src/host/feedreader.cpp
    src/host/feedreader.h
    src/host/feedtypes.h
    src/host/pipeio.cpp
    src/host/pipeio.h
    src/host/tracklyrics.cpp
    src/host/tracklyrics.h
    ../fooyin-plugin/src/feedwriter.cpp
    ../fooyin-plugin/src/feedwriter.h
)

target_include_directories(lyrics-app-feed-tests PRIVATE src/ ../fooyin-plugin/src)

target_compile_definitions(lyrics-app-feed-tests PRIVATE
    LX_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)

target_link_libraries(lyrics-app-feed-tests PRIVATE
    Qt6::Test
    ICU::uc
    ${LX_TAGLIB_TARGET}
)

add_test(NAME feed COMMAND lyrics-app-feed-tests)

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

# Tests for the LyricRenderer transition + scroll engines (headless/offscreen:
# fast line changes must not snap colors/zoom back, and the scroll must keep
# up with line changes that outpace the animation).
add_executable(lyrics-app-renderer-tests
    tests/tst_renderer.cpp
    src/renderer/lyricrenderer.cpp
)

target_include_directories(lyrics-app-renderer-tests PRIVATE src/)

target_link_libraries(lyrics-app-renderer-tests PRIVATE
    Qt6::Test
    Qt6::Widgets
)

add_test(NAME renderer COMMAND lyrics-app-renderer-tests)

# Tests for the lyric pipeline controller: an empty lyric shows the no-lyrics
# placeholder, an all-static lyric (invalid timestamps) shows its centered
# static lines, never a blank pane (headless/offscreen).
add_executable(lyrics-app-controller-tests
    tests/tst_controller.cpp
    src/app/appcontext.h
    src/app/lyriccontroller.cpp
    src/app/spectrumbridge.cpp
    src/app/spectrumtransport.cpp
    src/bridge/pausehide.cpp
    src/host/feedreader.cpp
    src/host/feedreader.h
    src/host/feedtypes.h
    src/host/tracklyrics.cpp
    src/host/tracklyrics.h
    src/window/lyricwindow.cpp
    src/renderer/lyricrenderer.cpp
    src/renderer/controlbar.cpp
    src/renderer/spectrumwidget.cpp
    src/settings/settingsdialog.cpp
    src/settings/colorpickerbutton.cpp
    src/config/desktoplyricconfig.cpp
    src/engine/lrcparser.cpp
    src/engine/wordparser.cpp
    src/engine/lyricselector.cpp
    src/engine/lyricclock.cpp
    src/engine/lyricplayer.cpp
    src/i18n/translationmanager.cpp
    translations.qrc
)

target_include_directories(lyrics-app-controller-tests PRIVATE src/)

target_link_libraries(lyrics-app-controller-tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    ICU::uc
    ${LX_TAGLIB_TARGET}
)

add_test(NAME controller COMMAND lyrics-app-controller-tests)
