# LX Lyrics — Fooyin plugin

## What it is

A plugin for [Fooyin Music Player](https://github.com/ludicrousDevelopment/Fooyin) (>= 0.11.1)
that drives the standalone `lyrics-app` display project.

Per `../../docs/protocol.md` (v2) the plugin is an **in-process adapter**:

- It observes playback through Fooyin's API and spawns `lx-lyrics-app --player-feed` as its
  **direct child** (pipes, never detached).
- It pushes playing context — file path, metadata, state, position, and analyser frames when
  Fooyin exposes them — as JSON lines on the child's stdin.
- It **never** reads lyric files, converts encodings, parses LRC, computes line numbers, or
  renders anything — the app owns acquisition, parsing, selection, and rendering.

## How it fits together

```
Fooyin playback (PlayerController) ──► PlayerBridge ──► FeedWriter ──spawn+pipes──► lx-lyrics-app --player-feed
Analyser (VisualisationService) ────► SpectrumSource ──┘                           (acquisition, parsing, rendering)
```

- **PlayerBridge** watches playback and turns track/state changes into `set_info` / `set_status` /
  `set_play` / `set_pause` / `set_stop` lines; `set_info` carries the file path and metadata — the
  app reads the lyrics itself.
- **SpectrumSource** pulls analyser magnitudes, log-scales them into 128 bytes per request.
- **FeedWriter** owns the child process: spawns it, writes the `hello` handshake and every
  host→app line, strictly parses the two app→host actions (`get_analyser_data_array`,
  `close_requested`), and reports the child's exit.
- The lyric window lives and dies with Fooyin: closing the window ends the session, and the app
  quits when its stdin closes (Fooyin exit or toggle off).

The app-side pipeline (acquisition, parsing, line selection, rendering, settings) is entirely the
`lyrics-app` project's job — see `../../lyrics-app/README.md`.

## Requirements

- Fooyin >= 0.11.1, **built with `INSTALL_HEADERS=ON`** so `FooyinConfig.cmake` and the Fooyin
  headers (`/usr/include/fooyin`) are installed. This tree is load-verified against the installed
  Fooyin 0.12.6 (plugin metadata + `Plugin`/`CorePlugin`/`GuiPlugin` interfaces resolve via
  `QPluginLoader`); rebuild against the Fooyin release you run, since compatibility is by ABI.
- Qt 6 >= 6.4 — Core, Widgets (FooyinConfig does not propagate Qt modules; declared here).
- A C++23 compiler, CMake >= 3.19, Ninja.
- The `lx-lyrics-app` binary (this repository's `lyrics-app`), on `PATH` or configured via AppPath.

No ICU and no WebSockets here: encoding conversion lives in the app, and the transport is a
private pipe pair.

## Build

```sh
cd plugins/fooyin
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Or use the project's `tools/install.sh` — this adapter is its default — to build and install the app and
the plugin, and to point `AppPath` at the binary without any manual entry.

Artifact: `build/fyplugin_lxlyrics.so`.

## Install

Copy the plugin into Fooyin's plugin directory, then restart Fooyin:

```sh
cp build/fyplugin_lxlyrics.so ~/.local/lib/fooyin/plugins/
# system-wide alternative:
# cp build/fyplugin_lxlyrics.so <prefix>/lib/fooyin/plugins/
```

After restart the plugin appears as **LX Lyrics** (Category: Lyrics) in Fooyin's plugin list.

## Use

- **View → Desktop Lyrics** — toggle. Spawns the standalone lyrics app
  (`lx-lyrics-app --player-feed`) as a direct child and pushes the current track right after the
  handshake, so the window shows what is already playing.
- **Settings → Lyrics → LX Lyrics**:
  - **AppPath** — path to the `lx-lyrics-app` binary. Empty = auto-detect: `PATH` lookup, then the
    plugin's app directory.
  - **RememberState** — remember the desktop lyrics state from the last session (kept under
    `LxLyrics/Enabled`). When on, startup restores the state the previous session ended with;
    when off, desktop lyrics always start off. A fresh install stays off until the user toggles
    **View → Desktop Lyrics** once.
  - **Open lyrics settings** — asks the running app to open its own configuration dialog
    (`open_settings`); no-op while the app is not running.
- **Lyrics** — the app reads them itself for the file at `set_info.path`: the same-name `.lrc`
  sidecar and the embedded tag (`LYRICS`, `SYNCEDLYRICS`, `UNSYNCEDLYRICS`, `UNSYNCED LYRICS`,
  plus the `LYRICS:<description>` fallbacks), joined when both exist. Encoding is auto-detected
  inside the app — UTF-8, UTF-16 (BOM), then GB18030→BIG5 via ICU.

## Troubleshooting

- **App not found / window never appears**: set **AppPath** to the full path of the `lx-lyrics-app`
  binary instead of relying on auto-detect.
- **No lyrics**: check the track's embedded tags, or drop a same-name `.lrc` beside the audio
  file. The file must be UTF-8 / UTF-16 / GB18030 / BIG5.
- **App aborts**: a protocol violation is fatal by design — the app logs the reason on stderr and
  exits non-zero, and the plugin turns the toggle off. The child's stderr is forwarded into
  Fooyin's log. A crash without a user close is respawned after ~1.5 s while the toggle is on.
- **Always-on-top under Wayland**: X11-native best effort; use KDE window rules
  (System Settings → Window Management → Window Rules) to force e.g. **Keep Above** for the
  lyrics-app window.

## License

GPL-3.0-only — this plugin links Fooyin's GPL-3.0 libraries. The lyrics rendering logic it drives
is ported from lx-music-desktop (Apache-2.0) and lives in the `lyrics-app` project; see the SPDX
header at the top of each source file.
