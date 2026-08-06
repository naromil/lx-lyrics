# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A port of lx-music-desktop's desktop-lyrics feature to native Qt6/C++23, split into **two independent CMake projects with zero shared source**. The WebSocket JSON protocol in `docs/protocol.md` is their only contract — change it whenever messages change.

- **`lyrics-app/`** — standalone Qt6 lyrics display window. Owns ALL parsing (LRC, lxlrc, tlrc/rlrc, `[awlrc:…]` container), line selection, karaoke rendering, and settings. Host-agnostic; does not know Fooyin exists.
- **`fooyin-plugin/`** — Fooyin (>= 0.11.1) plugin. Raw data + transport ONLY — acquires lyrics, converts encoding to UTF-8, watches playback, samples the analyser, and streams JSON frames over a loopback WebSocket. Never parses LRC or renders anything.
- **`references/`** — gitignored, read-only copy of lx-music-desktop 2.12.2 source. The parity reference: keep `desktopLyric.*` key names and lx-music semantics verbatim. Never edit.
- **`docs/`** — `architecture.md` (design), `protocol.md` (the shared contract), `research/` (engineering research notes).

## Build & verify (no CI, no lint config)

```sh
# lyrics-app
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# tests (4 QTest suites: engine 31 / lyricplayer 13 / protocol 11 / config 8 slots)
ctest --test-dir lyrics-app/build
ctest --test-dir lyrics-app/build -R engine          # single suite
./lyrics-app/build/lyrics-app-tests                  # single suite binary directly

# smoke test: run the demo; expect exit code 124 (timeout kill = no crash)
timeout 3 ./lyrics-app/build/lx-lyrics-app --demo

# fooyin-plugin (needs Fooyin built with INSTALL_HEADERS=ON + ICU)
cd fooyin-plugin && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
# artifact: build/fyplugin_lxlyrics.so -> ~/.local/lib/fooyin/plugins/

# build + install both, auto-configure Fooyin's AppPath/AutoSpawn (idempotent)
./tools/install.sh [--prefix DIR] [--no-autospawn]
```

App run modes: `--demo` (self-fed, exercises the full parse/render pipeline), `--ws=ws://127.0.0.1:PORT` (host-driven), add `--exit-on-disconnect` when spawned as a child of a host.

## lyrics-app internals

Source is under `src/`, layered roughly as: `engine/` (LRC/word parsers, `LyricPlayer` timed engine, `LyricSelector` lxlrc-vs-lrc choice + extended lyrics), `renderer/` (`LyricRenderer` widget, `ControlBar`, `SpectrumWidget`), `bridge/` (`WsClient`, pause-hide), `app/` (controller, spectrum bridge, CLI options), plus `config/`, `settings/`, `window/`, `i18n/`. Tests live in `tests/` as four QTest binaries (`tst_engine.cpp`, `tst_lyricplayer.cpp`, `tst_protocol.cpp`, `tst_config.cpp`), each built from the subset of sources it exercises.

- `main.cpp` forces `QT_QPA_PLATFORM=xcb` before QApplication (unless the env var is already set) — client-side `move()`/position restore only works under X11. Always-on-top/click-through remain compositor best-effort on Wayland (KDE window rules; see `tools/lx-lyrics.kwinrule`).
- Config persists to `~/.config/lx-lyrics/config.json` (37 keys, writes debounced 500 ms). Defaults are **user-tuned**, deviating from `references/src/common/defaultSetting.ts` on 8 keys (enable, isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width 300, fontSize 14, opacity 100, isZoomActiveLrc true; x/y default null = auto-position). They live ONLY in `DesktopLyricConfig::loadDefaults()` — keep `lyricrenderer.h` member initializers and the config tests in sync.
- When locked the window is transparent and click-through and the ControlBar is hidden — `Ctrl+,` reopens the settings dialog (documented escape hatch).

## fooyin-plugin internals

- `PlayerBridge` (playback events → `set_*` frames), `LyricSources` (embedded tags `LYRICS`/`SYNCEDLYRICS`/`UNSYNCEDLYRICS`, then sidecar `<trackdir>/<basename>.lrc`; encoding auto-detected UTF-8/UTF-16/GB18030→BIG5 via ICU), `SpectrumSource` (analyser → 128 log-scaled bytes), `HostServer` (loopback `QWebSocketServer`, strict single-client, strict protocol parsing), `AppSpawner` (`QProcess::startDetached` with `--ws` + `--exit-on-disconnect`).
- `lxlyrics.json.in` must keep **top-level capitalized keys** (`Name/Version/Category/...`, `Version` via `@PROJECT_VERSION@`) — wrapping them in an IID/MetaData/className object broke plugin discovery.
- FooyinConfig does NOT propagate WebSockets or ICU — both must be `find_package`'d explicitly in `fooyin-plugin/CMakeLists.txt`.

## Conventions & gotchas

- License split is intentional: `lyrics-app/` Apache-2.0 (ported logic), `fooyin-plugin/` GPL-3.0-only (links Fooyin). SPDX header at the top of every source file.
- Test fixtures `lyrics-app/tests/fixtures/sample.lrc` (UTF-8) and `sample-gbk.lrc` (same lyrics as GBK bytes) are the encoding-path e2e pair: `iconv -f GBK -t UTF-8 tests/fixtures/sample-gbk.lrc` must equal `sample.lrc`. Keep them in sync when touching the fixture.
- `build/` dirs are gitignored and shared across tasks; a concurrent LSP reconfiguration can transiently remove outputs — rebuild once before diagnosing. The LSP diagnostic "tst_config.moc not found" is pre-existing (AUTOMOC generates it at build time); ignore it.
- The repo owner handles pushes (often with tags); do not push unless explicitly asked.
