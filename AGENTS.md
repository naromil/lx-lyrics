# AGENTS.md

## Repository shape
Four independent projects with **zero shared C++ source**; the stdin/stdout player feed in `docs/protocol.md` (**v2**) is their only contract. Change it whenever messages change.

- `lyrics-app/` — standalone Qt6 desktop-lyrics display (port of lx-music-desktop's `renderer-lyric`). Owns ALL lyric acquisition (sidecar `.lrc` + embedded tags via TagLib, encoding conversion via ICU), parsing (LRC, lxlrc, tlrc/rlrc, the `[awlrc:…]` container), line selection (including karaoke word-tag parsing that drives line timing), line-by-line rendering (word tags are stripped; no per-word fill), and settings. The app is host-agnostic; it does not know Fooyin exists.
- `fooyin-plugin/` — Fooyin (>= 0.11.1) adapter. Playing context + transport only — watches playback events, samples the analyser, spawns `lx-lyrics-app --player-feed` as its **direct child**, and pushes `set_*` JSON lines; never acquires lyrics, parses LRC, or renders anything.
- `deadbeef-plugin/` — DeaDBeeF C adapter (`ddb_lxlyrics.so`); same feed contract, pure POSIX C feed thread, declared API floor **1.16** (deadbeef >= 1.9.3).
- `rhythmbox-plugin/` — Rhythmbox libpeas Python adapter; spawns the app via `Gio.Subprocess`; no analyser API (`spectrum: false`).
- `references/` — gitignored, read-only copy of lx-music-desktop 2.12.2 source. The parity reference: keep `desktopLyric.*` key names and lx-music semantics verbatim. Never edit.
- `docs/` — `architecture.md` (design), `protocol.md` (the shared contract, v2), `research/` (engineering research notes).

Dropped players: Strawberry, Clementine, Elisa and Tauon have **no extension mechanism** — dropped per decision (no MPRIS fallback, no forks). Audacious / Quod Libet / VLC adapters are out of scope for this increment.

## Build & verify (no CI)
```sh
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build                 # 6 suites: engine(44) lyricplayer(33) feed(33) config(8) renderer(21) controller(32) — 171 slots
ctest --test-dir build -R engine       # single suite
QT_QPA_PLATFORM=offscreen timeout 3 ./build/lx-lyrics-app --demo # expect exit code 124 (timeout kill = no crash)
printf '{"v":2,"action":"nonsense"}\n' | QT_QPA_PLATFORM=offscreen ./build/lx-lyrics-app --player-feed; echo "exit=$?" # expect exit=1 and a protocol-error log line
```
- The headless demo check MUST set `QT_QPA_PLATFORM=offscreen`: `main.cpp` forces `xcb`, so on a box with no display server a bare `env -u DISPLAY … --demo` aborts (exit 134, Qt xcb "could not connect to display"). Use `xvfb-run -a … --demo` when the check must run on the user's real platform (xcb).
- `./tools/lint.sh [--fix] [--format-only] [--tidy-only]` is the style/static-analysis gate, and it is **C++-only**: clang-format over `lyrics-app/src`, `lyrics-app/tests` and `fooyin-plugin/src` (`.cpp`/`.h`), plus clang-tidy via `run-clang-tidy` over the `lyrics-app` and `fooyin-plugin` build dirs (it strips Qt's GCC-only `-mno-direct-extern-access`, which the clang driver behind clang-tidy otherwise rejects). Findings fail the run. `deadbeef-plugin/` (C) and `rhythmbox-plugin/` (Python) are **outside this gate**: keep the DeaDBeeF sources clang-format-clean under the repo `.clang-format` by hand (they pass `clang-format --dry-run -Werror` today); the Python adapter has no style gate — its hermetic unittest suite is its check.
- Suites are separate binaries built from the subset of sources each exercises: `build/lyrics-app-tests` (engine), `lyrics-app-lyricplayer-tests`, `-feed-`, `-config-`, `-renderer-`, `-controller-`; run one directly to bypass ctest. The `feed` suite compiles the app's `FeedReader` AND the plugin's Fooyin-free `fooyin-plugin/src/feedwriter.cpp` — both halves of the wire contract.
- `tests/testbootstrap.h` pins `QT_QPA_PLATFORM=offscreen` for the widget suites (renderer, controller) by odr-using `kForceOffscreen`; they must keep passing with no display server (`env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=$(mktemp -d)` is the check).
- App run modes: `--player-feed` (host-driven — an adapter spawns the app as its direct child; EOF on stdin quits immediately), `--demo` (self-fed — fake track AND a synthetic spectrum transport, so the visualizer renders with no host), no flag = inert window. `--ws`/`--exit-on-disconnect` no longer exist.
- fooyin-plugin needs Fooyin built with `INSTALL_HEADERS=ON`; artifact `build/fyplugin_lxlyrics.so`, installed to `<prefix>/lib/fooyin/plugins`. It has **no test suite of its own** — the app's `feed` suite compiles its Fooyin-free `feedwriter.cpp` as the host half of the wire.
- deadbeef-plugin's configure step **requires** DeaDBeeF headers from a 1.10.1 checkout (`-DDEADBEEF_INCLUDE_DIR=…`; a bare `cmake -B build -G Ninja` FATAL_ERRORs) because the plugin compiles against them — the `feed_test` target is the header-free part (it builds only `feed.c` + `json.c`), so `ctest --test-dir build` runs with no DeaDBeeF installed. Install `ddb_lxlyrics.so` to `~/.local/lib/deadbeef/`, then restart the player. API floor 1.16 (deadbeef >= 1.9.3) — see its README for the upstream header proof.
- rhythmbox-plugin installs by copying its directory to `~/.local/share/rhythmbox/plugins/lxlyrics/`; tests run hermetically with `python3 -m unittest discover -s rhythmbox-plugin/tests -v` (39 tests); PyGObject/girepository notes in its README.
- `build/` dirs are gitignored and shared across tasks; a concurrent LSP reconfiguration can transiently remove outputs — rebuild once before diagnosing.
- The LSP diagnostic "tst_config.moc not found" is pre-existing (AUTOMOC generates it at build time); ignore it.
- Always rebuild after any code modification — never report a change as done until `cmake --build build` succeeds.

## lyrics-app layout
`src/` is layered as: `engine/` (LRC/word parsers, `LyricPlayer` timed engine, `LyricSelector` lxlrc-vs-lrc choice + extended lyrics), `renderer/` (`LyricRenderer` widget, `ControlBar`, `SpectrumWidget`), `host/` (`FeedReader` v2 stdin/stdout protocol, `TrackLyrics` acquisition, `PipeIO` pipe devices), `bridge/` (pause-hide), `app/` (controller, spectrum bridge + transport, CLI options), plus `config/`, `settings/`, `window/`, `i18n/`. Tests live in `tests/` as six QTest binaries.

## App quirks
- `main.cpp` forces `QT_QPA_PLATFORM=xcb` before QApplication unless the env var is already set — client-side `move()`/position restore only works under X11. Set `QT_QPA_PLATFORM` beforehand to override. Always-on-top/click-through remain compositor best-effort on Wayland (KDE window rules: `tools/lx-lyrics.kwinrule`).
- `main.cpp` also forces `QT_FORCE_STDERR_LOGGING=1` unless set: Qt >= 6.11 silences `qInfo`/`qWarning` when stderr is a pipe — and the feed's stderr IS a pipe by design. Respect an explicit override.
- The app ignores `SIGPIPE` (`main.cpp`, and again in `host/pipeio.cpp` so the class is safe without a `main`) and writes its **stdout** non-blocking with a bounded budget: a host that stops draining (or closes) the app's stdout gets **one** `qWarning` and every later app→host line is dropped — the app keeps running, because **stdin EOF is the only quit trigger** (a dead stdout must never end a session).
- Config persists to `~/.config/lx-lyrics/config.json` (37 keys, writes debounced 500 ms). Defaults are **user-tuned**, deviating from `references/src/common/defaultSetting.ts` on 8 keys: enable, isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width(300), fontSize(14), opacity(100), isZoomActiveLrc(true); x/y default null = auto-position. They live ONLY in `DesktopLyricConfig::loadDefaults()` — keep `lyricrenderer.h` member initializers and the config tests in sync.
- When locked the window is transparent and click-through and the ControlBar is hidden — `Ctrl+,` reopens the settings dialog (documented escape hatch).

## Plugin quirks
- `PlayerBridge` (playback events → `set_*` lines with `path` + metadata + state), `SpectrumSource` (analyser → 128 log-scaled bytes), `FeedWriter` (spawns `lx-lyrics-app --player-feed` as a direct child; writes the `hello` handshake and all host→app lines; strict-parses the two app→host actions; forwards the child's stderr). The app owns acquisition — the plugin has no lyric source, no socket server, and no detached spawn.
- `feedwriter.cpp` must stay **Fooyin-header-free**: the app's `feed` suite compiles it as the host side of the wire.
- `lxlyrics.json.in` must keep **top-level capitalized keys** (`Name/Version/Category/...`, `Version` via `@PROJECT_VERSION@`). Wrapping them in an IID/MetaData/className object broke plugin discovery in Fooyin.
- FooyinConfig does NOT propagate Qt modules (only Core/Widgets/Sql/Concurrent/Network) — the plugin declares its Qt6 Core/Widgets dependency in `fooyin-plugin/CMakeLists.txt`. ICU/WebSockets are no longer needed on the plugin side.
- `deadbeef-plugin/`: `lxlyrics.enabled` **is** the toggle state (DeaDBeeF's action API has no checkable flag, so the menu item carries no tick) and it is honoured at player start (`DB_EV_PLUGINSLOADED` → `restore_session()`, which clears the key if the session cannot start). The reader parses app→host lines strictly — a wrong `v`, an unknown action, a malformed payload or an over-1 MiB line is a protocol error that ends the session, while a line that is not a JSON object is skipped loudly. The write end is `O_NONBLOCK` with a bounded `poll()` wait, and the feed object is reference-counted because `message()` runs on DeaDBeeF's player thread while the menu action runs on the GTK main thread.
- `rhythmbox-plugin/`: the child's stdin is made non-blocking (`os.set_blocking`) so an app that stops draining cannot park Rhythmbox's main loop, and the pending queue is capped at `MAX_PENDING_LINES` — the oldest stale lines are dropped with one log per overflow episode, never the in-flight line or the leading `hello`. A well-formed line with a wrong `v` or an unknown action is a protocol error (§7): log, kill the child, uncheck the toggle. `stop()` closes the child's stdin and escalates to `force_exit()` only after `STOP_GRACE_MS`.

## Test fixtures
`lyrics-app/tests/fixtures/sample.lrc` (UTF-8) and `sample-gbk.lrc` (same lyrics as GBK bytes) are the encoding-path e2e fixtures: `iconv -f GBK -t UTF-8 tests/fixtures/sample-gbk.lrc` must equal `sample.lrc` (the `feed` suite asserts exactly that). `embedded-lyrics.flac` carries `LYRICS=[00:00.00]Embedded line one\n[00:01.00]Embedded line two` for the embedded-tag path. Keep them in sync when touching the fixtures.

## Conventions
- License split is intentional: `lyrics-app/` Apache-2.0 (ported logic); `fooyin-plugin/`, `deadbeef-plugin/`, `rhythmbox-plugin/` GPL-3.0-only (a repo decision the hosts permit — Fooyin is GPL-3.0, Rhythmbox is GPL-2.0-or-later, whose "or later" clause allows a GPL-3.0 plugin, and DeaDBeeF's plugin API header is zlib-licensed). SPDX header at the top of every source file.
- `./tools/install.sh [--prefix DIR] [--no-autospawn]` builds the app and the Fooyin plugin in Release, installs the app to `<prefix>/bin` and the plugin to `<prefix>/lib/fooyin/plugins` (default `~/.local`), and patches `fooyin.conf` `[LxLyrics]` `AppPath`/`RememberState` in place (idempotent; `--no-autospawn` writes `RememberState=false`, and a stale `AutoSpawn=` line is deleted). It covers ONLY the app + fooyin plugin; the DeaDBeeF/Rhythmbox adapters install per their own READMEs.
- The repo owner handles pushes (often with tags); do not push unless explicitly asked.
