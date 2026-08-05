# AGENTS.md

## Repository shape
Two independent CMake/C++23 projects with **zero shared source**; the WebSocket JSON protocol in `docs/protocol.md` is their only contract. Change it whenever messages change.

- `lyrics-app/` — standalone Qt6 desktop-lyrics display (port of lx-music-desktop's `renderer-lyric`). Owns ALL parsing, line selection, rendering, and settings. The app is host-agnostic.
- `fooyin-plugin/` — Fooyin (>= 0.11.1) plugin. Raw data + transport only — never parses LRC or renders anything.
- `references/` — gitignored, read-only copy of lx-music-desktop 2.12.2 source. The parity reference: keep `desktopLyric.*` key names and lx-music semantics verbatim. Never edit.

## Build & verify (no CI, no lint config)
```sh
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build                 # 4 suites: engine(31) lyricplayer(13) protocol(11) config(8)
timeout 3 ./build/lx-lyrics-app --demo # expect exit code 124 (timeout kill = no crash)
```
- fooyin-plugin needs Fooyin built with `INSTALL_HEADERS=ON` and ICU; artifact is `build/fyplugin_lxlyrics.so`.
- `build/` dirs are gitignored and shared across tasks; a concurrent LSP reconfiguration can transiently remove outputs — rebuild once before diagnosing.
- The LSP diagnostic "tst_config.moc not found" is pre-existing (AUTOMOC generates it at build time); ignore it.

## App quirks
- `main.cpp` forces `QT_QPA_PLATFORM=xcb` before QApplication unless the env var is already set — client-side `move()`/position restore only works under X11. Set `QT_QPA_PLATFORM` beforehand to override. Always-on-top/click-through remain compositor best-effort on Wayland.
- Config persists to `~/.config/lx-lyrics/config.json` (36 keys, writes debounced 500 ms). Defaults are **user-tuned**, deviating from `references/src/common/defaultSetting.ts` on 8 keys: enable, isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width(300), fontSize(14), opacity(100), isZoomActiveLrc(true); x/y default null = auto-position. They live ONLY in `DesktopLyricConfig::loadDefaults()` — keep `lyricrenderer.h` member initializers and the config tests in sync.
- When locked the window is transparent and click-through and the ControlBar is hidden — `Ctrl+,` reopens the settings dialog (documented escape hatch).

## Plugin quirks
- `lxlyrics.json.in` must keep **top-level capitalized keys** (`Name/Version/Category/...`, `Version` via `@PROJECT_VERSION@`). Wrapping them in an IID/MetaData/className object broke plugin discovery in Fooyin.
- FooyinConfig does NOT propagate WebSockets or ICU — both must be `find_package`'d explicitly in `fooyin-plugin/CMakeLists.txt`.

## Test fixtures
`lyrics-app/tests/fixtures/sample.lrc` (UTF-8) and `sample-gbk.lrc` (same lyrics as GBK bytes) are the encoding-path e2e fixtures: `iconv -f GBK -t UTF-8 tests/fixtures/sample-gbk.lrc` must equal `sample.lrc`. Keep them in sync when touching the fixture.

## Conventions
- License split is intentional: `lyrics-app/` Apache-2.0 (ported logic), `fooyin-plugin/` GPL-3.0-only (links Fooyin). SPDX header at the top of every source file.
- `./tools/install.sh` builds both in Release, installs them, and patches `fooyin.conf` `[LxLyrics] AppPath`/`AutoSpawn` in place (idempotent).
- The repo owner handles pushes (often with tags); do not push unless explicitly asked.
