# LX Lyrics — standalone display app

## What it is

A standalone desktop lyrics display for Linux, written in Qt6 / C++23. It is the desktop-lyrics
feature of [lx-music-desktop](https://github.com/lyswhut/lx-music-desktop) (`renderer-lyric` /
`lyric-font-player`) extracted into a self-contained widget application.

The app is **host-agnostic**: it knows nothing about Fooyin or any specific player. A host drives
it over the WebSocket JSON protocol documented in `../docs/protocol.md`; the app owns **all**
parsing, line selection, and rendering.

## Features

- **Line-by-line synchronized lyrics** with active-line highlight. Karaoke word-fill is
  intentionally **not** included, matching lx-music-desktop's original desktop-lyric design.
- **Horizontal and vertical** layouts (`desktopLyric.direction`).
- **Full port of lx-music's desktop-lyric settings**: played/unplayed/shadow colors, font family,
  font size (10–80), window opacity (6–100%), line gap (0–25 px), text align, scroll align,
  ellipsis on overflow, active-line zoom, font-weight toggles, window flags (lock, always-on-top,
  show in taskbar, hover-hide, fullscreen-hide), and a reset-to-defaults action.
- **Control bar**: close, lock, font size ±, opacity ±, zoom, always-on-top.
- **i18n**: zh-cn, zh-tw, en-us.
- **Spectrum visualizer**: 128 bars, host-fed over the protocol.
- **Pause-hide**: the window hides while playback is paused.
- Drag to move, resize, lock, always-on-top.

## Build

Prerequisites:

- Qt 6 >= 6.4 — Widgets, Network, WebSockets
- CMake >= 3.21 (as declared in `CMakeLists.txt`), Ninja
- A C++23 compiler

```sh
cd lyrics-app
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run modes

```sh
./build/lyrics-app --demo                                    # self-fed demo
./build/lyrics-app --ws=ws://127.0.0.1:PORT                  # host-driven
./build/lyrics-app --ws=ws://127.0.0.1:PORT --exit-on-disconnect
```

- `--demo` — no host. The app self-feeds a fake track through the **same** pipeline as a host
  `set_info` message, so the full parse/render path is exercised.
- `--ws=ws://127.0.0.1:PORT` — connect to a host implementing `docs/protocol.md`. With
  `--exit-on-disconnect` the app quits when the host closes the socket (used when a host spawns
  it as a child process).
- Without `--ws` the app runs standalone with its empty (demo-ish) window — there is no host to
  feed it, so the lyric area stays blank.

## Config

Settings persist to `~/.config/lx-lyrics/config.json` (per `QStandardPaths::ConfigLocation`).
There are 36 keys total — 30 `desktopLyric.*` keys plus 6 display-affecting `common.*` /
`player.*` keys. Condensed defaults:

| Group | Keys (default) |
|---|---|
| Window | `enable`(false) `isLock`(false) `isAlwaysOnTop`(false) `isAlwaysOnTopLoop`(false) `isShowTaskbar`(false) `pauseHide`(true) `audioVisualization`(false) `fullscreenHide`(true) `isDelayScroll`(true) `isLockScreen`(platform) `isHoverHide`(false) `width`(450) `height`(300) `x`/`y`(null = auto-position) |
| Layout | `direction`(horizontal) `scrollAlign`(center) |
| Style | `align`(center) `font`("") `fontSize`(20) `lineGap`(15) `lyricUnplayColor`(rgba(255,255,255,1)) `lyricPlayedColor`(rgba(7,197,86,1)) `lyricShadowColor`(rgba(0,0,0,0.18)) `opacity`(95) `ellipsis`(false) `isZoomActiveLrc`(false) `isFontWeightFont`(true) `isFontWeightLine`(true) `isFontWeightExtended`(true) |
| Player | `common.langId`(null) `player.isShowLyricTranslation`(false) `player.isShowLyricRoma`(false) `player.isSwapLyricTranslationAndRoma`(false) `player.isPlayLxlrc`(platform) `player.playbackRate`(1.0) |

Writes are debounced (500 ms) so rapid settings changes do not thrash the disk.

## Settings dialog

Press **`Ctrl+,`** to open the settings dialog (the control bar hides when the window is locked,
so the shortcut is the way back). Every change writes through the config and re-renders live.

## Tests

Four QTest suites (63 slots total), run with CTest:

```sh
ctest --test-dir build
```

| Suite | Binary | Slots |
|---|---|---|
| engine | `lyrics-app-tests` | 31 |
| lyricplayer | `lyrics-app-lyricplayer-tests` | 13 |
| protocol | `lyrics-app-protocol-tests` | 11 |
| config | `lyrics-app-config-tests` | 8 |

**Test fixtures**: `tests/fixtures/sample.lrc` (UTF-8) and `tests/fixtures/sample-gbk.lrc` (the
same lyrics encoded as GBK bytes) are rerunnable e2e fixtures for the encoding path:
`iconv -f GBK -t UTF-8 tests/fixtures/sample-gbk.lrc` must equal `tests/fixtures/sample.lrc`.

## Wayland note

Always-on-top, click-through, and window transparency are X11-native best effort. Under a Wayland
compositor Qt has no standard way to raise the window above others — use KDE window rules
(System Settings → Window Management → Window Rules) to force e.g. **Keep Above** for the app.

## License

Apache-2.0. Lyric parsing/rendering logic is ported from lx-music-desktop (Apache-2.0) — see the
SPDX header at the top of each source file.
