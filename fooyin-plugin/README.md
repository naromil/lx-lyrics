# LX Lyrics — Fooyin plugin

## What it is

A plugin for [Fooyin Music Player](https://github.com/ludicrousDevelopment/Fooyin) (>= 0.11.1)
that drives the standalone `lyrics-app` display project.

Per `../docs/protocol.md` the plugin is a **raw-data + transport provider only**:

- It acquires raw lyrics, converts encoding to UTF-8, watches playback, and samples the analyser.
- It pushes track/lyrics/playback/spectrum frames to the app over a loopback WebSocket.
- It **never** parses LRC, computes line numbers, builds extended lyrics, or renders anything —
  all parsing, selection, and rendering live in the `lyrics-app` project.

## How it fits together

```
Fooyin playback (PlayerController)  ─┐
Track tags + sidecar .lrc (LyricSources) ─┼─► HostServer ── WebSocket ──► lyrics-app
Analyser (VisualisationService)     ─┘   (JSON frames + 128-byte binary)     (all parsing/rendering)
```

- **PlayerBridge** watches playback and turns track/state changes into `set_*` frames.
- **LyricSources** reads embedded tags then the sidecar `.lrc`, decoding encoding exactly once.
- **SpectrumSource** pulls analyser magnitudes, log-scales them into 128 bytes per request.
- **HostServer** owns the loopback socket and strictly parses the three app→host requests.
- **AppSpawner** starts the lyrics app (`QProcess::startDetached`) with `--ws` + `--exit-on-disconnect`.

The app-side pipeline (parsing, line selection, rendering, settings) is entirely the
`lyrics-app` project's job — see `../lyrics-app/README.md`.

## Requirements

- Fooyin >= 0.11.1, **built with `INSTALL_HEADERS=ON`** so `FooyinConfig.cmake` and the Fooyin
  headers (`/usr/include/fooyin`) are installed.
- Qt 6 >= 6.4 — Widgets, WebSockets (FooyinConfig does not propagate WebSockets; declared here).
- ICU (`libicuuc`) — for lyric encoding conversion (GB18030 / BIG5).
- A C++23 compiler, CMake >= 3.19, Ninja.

## Build

```sh
cd fooyin-plugin
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Or use the project's `tools/install.sh` to build and install both components automatically.

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
  (`--ws=ws://127.0.0.1:PORT --exit-on-disconnect`) and feeds it the current track.
- **Settings → Lyrics → LX Lyrics**:
  - **AppPath** — path to the `lx-lyrics-app` binary. Empty = auto-detect: `PATH` lookup, then the
    plugin's app directory.
  - **AutoSpawn** — start desktop lyrics when Fooyin starts.
- **Lyric sources** (priority order):
  1. Embedded tags: `LYRICS`, `SYNCEDLYRICS`, `UNSYNCEDLYRICS`, `UNSYNCED LYRICS`.
  2. Sidecar file `<trackdir>/<basename>.lrc` (e.g. `song.mp3` → `song.lrc`).
  - Encoding is auto-detected at the file boundary — UTF-8, UTF-16 (BOM), then
    GB18030→BIG5 via ICU — and converted to UTF-8 before anything is sent.

## Troubleshooting

- **App not found / window never appears**: set **AppPath** to the full path of the `lx-lyrics-app`
  binary instead of relying on auto-detect.
- **No lyrics**: check the track's embedded tags, or drop a same-name `.lrc` beside the audio
  file. The file must be UTF-8 / UTF-16 / GB18030 / BIG5.
- **Always-on-top under Wayland**: X11-native best effort; use KDE window rules
  (System Settings → Window Management → Window Rules) to force e.g. **Keep Above** for the
  lyrics-app window.

## License

GPL-3.0-only — this plugin links Fooyin's GPL-3.0 libraries. The lyrics rendering logic it drives
is ported from lx-music-desktop (Apache-2.0) and lives in the `lyrics-app` project; see the SPDX
header at the top of each source file.
