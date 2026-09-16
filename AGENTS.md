# AGENTS.md

## Repository shape
Two independent CMake/C++23 projects with **zero shared source**; the WebSocket JSON protocol in `docs/protocol.md` is their only contract. Change it whenever messages change.

- `lyrics-app/` — standalone Qt6 desktop-lyrics display (port of lx-music-desktop's `renderer-lyric`). Owns ALL parsing (LRC, lxlrc, tlrc/rlrc, the `[awlrc:…]` container), line selection (including karaoke word-tag parsing that drives line timing), line-by-line rendering (word tags are stripped; no per-word fill), and settings. The app is host-agnostic; it does not know Fooyin exists.
- `fooyin-plugin/` — Fooyin (>= 0.11.1) plugin. Raw data + transport only — acquires lyrics, converts them to UTF-8, watches playback events, samples the analyser, and streams JSON frames over a loopback WebSocket; never parses LRC or renders anything.
- `references/` — gitignored, read-only copy of lx-music-desktop 2.12.2 source. The parity reference: keep `desktopLyric.*` key names and lx-music semantics verbatim. Never edit.
- `docs/` — `architecture.md` (design), `protocol.md` (the shared contract), `research/` (engineering research notes).

## Build & verify (no CI)
```sh
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build                 # 6 suites: engine(44) lyricplayer(33) protocol(16) config(8) renderer(21) controller(29)
ctest --test-dir build -R engine       # single suite
timeout 3 ./build/lx-lyrics-app --demo # expect exit code 124 (timeout kill = no crash)
```
- `./tools/lint.sh [--fix] [--format-only] [--tidy-only]` is the style/static-analysis gate: clang-format check over all sources plus clang-tidy via `run-clang-tidy` over both projects' build dirs (it strips Qt's GCC-only `-mno-direct-extern-access`, which the clang driver behind clang-tidy otherwise rejects). Findings fail the run.
- Suites are separate binaries built from the subset of sources each exercises: `build/lyrics-app-tests` (engine), `lyrics-app-lyricplayer-tests`, `-protocol-`, `-config-`, `-renderer-`, `-controller-`; run one directly to bypass ctest.
- `tests/testbootstrap.h` pins `QT_QPA_PLATFORM=offscreen` for the widget suites (renderer, controller) by odr-using `kForceOffscreen`; they must keep passing with no display server (`env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=$(mktemp -d)` is the check).
- App run modes: `--demo` (self-fed, exercises the full parse/render pipeline), `--ws=ws://127.0.0.1:PORT` (host-driven), plus `--exit-on-disconnect` when spawned as a child of a host.
- fooyin-plugin needs Fooyin built with `INSTALL_HEADERS=ON` and ICU; artifact is `build/fyplugin_lxlyrics.so`.
- `build/` dirs are gitignored and shared across tasks; a concurrent LSP reconfiguration can transiently remove outputs — rebuild once before diagnosing.
- The LSP diagnostic "tst_config.moc not found" is pre-existing (AUTOMOC generates it at build time); ignore it.
- Always rebuild after any code modification — never report a change as done until `cmake --build build` succeeds.

## lyrics-app layout
`src/` is layered as: `engine/` (LRC/word parsers, `LyricPlayer` timed engine, `LyricSelector` lxlrc-vs-lrc choice + extended lyrics), `renderer/` (`LyricRenderer` widget, `ControlBar`, `SpectrumWidget`), `bridge/` (`WsClient`, pause-hide), `app/` (controller, spectrum bridge, CLI options), plus `config/`, `settings/`, `window/`, `i18n/`. Tests live in `tests/` as six QTest binaries.

## App quirks
- `main.cpp` forces `QT_QPA_PLATFORM=xcb` before QApplication unless the env var is already set — client-side `move()`/position restore only works under X11. Set `QT_QPA_PLATFORM` beforehand to override. Always-on-top/click-through remain compositor best-effort on Wayland (KDE window rules: `tools/lx-lyrics.kwinrule`).
- Config persists to `~/.config/lx-lyrics/config.json` (37 keys, writes debounced 500 ms). Defaults are **user-tuned**, deviating from `references/src/common/defaultSetting.ts` on 8 keys: enable, isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width(300), fontSize(14), opacity(100), isZoomActiveLrc(true); x/y default null = auto-position. They live ONLY in `DesktopLyricConfig::loadDefaults()` — keep `lyricrenderer.h` member initializers and the config tests in sync.
- When locked the window is transparent and click-through and the ControlBar is hidden — `Ctrl+,` reopens the settings dialog (documented escape hatch).

## Plugin quirks
- `PlayerBridge` (playback events → `set_*` frames), `LyricSources` (embedded tags `LYRICS`/`SYNCEDLYRICS`/`UNSYNCEDLYRICS`, then sidecar `<trackdir>/<basename>.lrc`; encoding auto-detected UTF-8/UTF-16/GB18030→BIG5 via ICU), `SpectrumSource` (analyser → 128 log-scaled bytes), `HostServer` (loopback `QWebSocketServer`, strict single-client, strict protocol parsing), `AppSpawner` (`QProcess::startDetached` with `--ws` + `--exit-on-disconnect`).
- `lxlyrics.json.in` must keep **top-level capitalized keys** (`Name/Version/Category/...`, `Version` via `@PROJECT_VERSION@`). Wrapping them in an IID/MetaData/className object broke plugin discovery in Fooyin.
- FooyinConfig does NOT propagate WebSockets or ICU — both must be `find_package`'d explicitly in `fooyin-plugin/CMakeLists.txt`.

## Test fixtures
`lyrics-app/tests/fixtures/sample.lrc` (UTF-8) and `sample-gbk.lrc` (same lyrics as GBK bytes) are the encoding-path e2e fixtures: `iconv -f GBK -t UTF-8 tests/fixtures/sample-gbk.lrc` must equal `sample.lrc`. Keep them in sync when touching the fixture.

## Conventions
- License split is intentional: `lyrics-app/` Apache-2.0 (ported logic), `fooyin-plugin/` GPL-3.0-only (links Fooyin). SPDX header at the top of every source file.
- `./tools/install.sh [--prefix DIR] [--no-autospawn]` builds both in Release, installs the app to `<prefix>/bin` and the plugin to `<prefix>/lib/fooyin/plugins` (default `~/.local`), and patches `fooyin.conf` `[LxLyrics] AppPath`/`AutoSpawn` in place (idempotent).
- The repo owner handles pushes (often with tags); do not push unless explicitly asked.
