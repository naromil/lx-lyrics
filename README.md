# lx-lyrics

A standalone desktop lyrics feature extracted from [lx-music-desktop](https://github.com/lyswhut/lx-music-desktop): a self-contained lyrics display app plus in-process player adapters that drive it.

## Status

Seven projects — the display app plus six adapters — with **zero shared C++ source**; the only contract is `docs/protocol.md` **v2** — a stdin/stdout player feed between a player-side adapter and the display app.

- **`lyrics-app/`** — standalone Qt6 / C++23 desktop lyrics window with synchronized scrolling and active-line rendering. Owns lyric acquisition (sidecar `.lrc` + embedded tags), parsing, selection, and rendering. Passes its full test suite (6 suites, 171 QTest slots). Runs host-driven (`--player-feed`), self-fed (`--demo`), or as an inert window.
- **`plugins/fooyin/`** — Fooyin adapter (>= 0.11.1): spawns `lx-lyrics-app --player-feed` as its direct child and pushes playing context (path, metadata, state, position, spectrum) over the feed. Plugin metadata and the `Plugin`/`CorePlugin`/`GuiPlugin` interfaces load-verified against the installed Fooyin 0.12.6.
- **`plugins/deadbeef/`** — DeaDBeeF C adapter (`ddb_lxlyrics.so`), API floor 1.16 (DeaDBeeF >= 1.9.3); answers spectrum requests when the player exposes its analyser.
- **`plugins/rhythmbox/`** — Rhythmbox libpeas Python adapter; Rhythmbox has no analyser API, so it declares `spectrum: false`.
- **`plugins/audacious/`** — Audacious general plugin (`lxlyrics.so`, ≥ 4.6.1; pure POSIX C transport + C++17 glue): spawns the child and pushes playing context, and answers analyser requests through Audacious' public `Visualizer` API (`spectrum: true`).
- **`plugins/quodlibet/`** — Quod Libet event plugin (single Python module `lxlyrics.py`, ≥ 4.4.0): spawns the child on the GLib main loop and pushes playing context plus `set_fullscreen`; Quod Libet exposes no analyser, so it declares `spectrum: false`.
- **`plugins/vlc/`** — VLC 3.0.x interface module (`liblxlyrics_plugin.so`, loaded via `--extraintf=lxlyrics`): a poll-based sampler feeds the child; no analyser is reachable from a plugin, so `spectrum: false`; VLC 4 is unsupported.

## Player support

Adapters exist for Fooyin, DeaDBeeF, Rhythmbox, Audacious, Quod Libet and VLC. **Strawberry, Clementine, Elisa and Tauon are dropped** — they have no extension mechanism to host an in-process adapter, and per decision there is no MPRIS fallback and no fork.

## Repository layout

| Path | Contents |
|---|---|
| `lyrics-app/` | standalone Qt6 lyrics display — see `lyrics-app/README.md` |
| `plugins/fooyin/` | Fooyin adapter — see `plugins/fooyin/README.md` |
| `plugins/deadbeef/` | DeaDBeeF adapter — see `plugins/deadbeef/README.md` |
| `plugins/rhythmbox/` | Rhythmbox adapter — see `plugins/rhythmbox/README.md` |
| `plugins/audacious/` | Audacious adapter — see `plugins/audacious/README.md` |
| `plugins/quodlibet/` | Quod Libet adapter — see `plugins/quodlibet/README.md` |
| `plugins/vlc/` | VLC adapter — see `plugins/vlc/README.md` |
| `docs/` | architecture, protocol, and research summaries |
| `references/` | lx-music-desktop v2.12.2 source (gitignored; read-only reference) |
| `tools/` | build/install helpers — `tools/install.sh` builds and installs the app plus any of the six adapters (`--player NAME`, `--player all`); `tools/lint.sh` runs the clang-format + clang-tidy gate |

## Documentation

- `lyrics-app/README.md` — build, run modes, config, tests
- `plugins/fooyin/README.md` — build, install, usage, troubleshooting
- `plugins/deadbeef/README.md`, `plugins/rhythmbox/README.md`, `plugins/audacious/README.md`, `plugins/quodlibet/README.md`, `plugins/vlc/README.md` — adapter build/install/test notes
- `docs/architecture.md` — component design, ownership invariant, and decoupling boundary
- `docs/protocol.md` — the v2 player feed (stdin/stdout JSON lines; the shared contract)
- `docs/research/` — condensed engineering research for the port

## Quick start

```sh
./tools/install.sh                        # the app + every adapter whose player is installed here
./tools/install.sh --player all           # the app + all six adapters, detected or not
./tools/install.sh --player vlc --player audacious
```

With no `--player` flag the installer takes every adapter whose player it finds on
the machine; name adapters to pick them explicitly, or `--player all` to install all
six. Each run builds the app plus the selected adapters in Release, installs each one
into its own player's plugin directory, and writes that adapter's config so it finds
the installed `lx-lyrics-app` (Fooyin `AppPath`, DeaDBeeF `lxlyrics.app_path`,
Rhythmbox `app-path`, Audacious `[lx-lyrics] app_path`, Quod Libet
`lxlyrics_app_path`, VLC `lxlyrics-app-path`). `--prefix DIR` moves the app to
`DIR/bin` and nothing else — a plugin only moves with its own
`--fooyin-plugin-dir`/`--audacious-plugin-dir`/`--vlc-plugin-dir`; `--no-autospawn`
writes each player's "do not start the lyrics session on your own" state.
`./tools/install.sh --help` lists the per-adapter header/destination overrides.
Adapters that install into a root-owned directory (a distro Audacious, a system VLC
plugin dir) print the exact `sudo install` command and exit non-zero instead.

Manual fallback (the same steps by hand):

```sh
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build        # 1. build the display app
cd ../plugins/fooyin && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build # 2. build the Fooyin adapter
cp build/fyplugin_lxlyrics.so ~/.local/lib/fooyin/plugins/                                       # 3. install, restart Fooyin
# 4. point the plugin at the app binary (Settings -> Lyrics -> LX Lyrics -> AppPath), or put
#    lyrics-app/build/lx-lyrics-app on your PATH so auto-detect finds it
```

Adapter preconditions at a glance (the ones a plain `--player all` run reports when
they are missing):

- **Fooyin** — Fooyin built with `INSTALL_HEADERS=ON`; artifact `build/fyplugin_lxlyrics.so` → `<prefix>/lib/fooyin/plugins` (or `~/.local/lib/fooyin/plugins`).
- **DeaDBeeF** — headers from a 1.10.1 checkout (`--deadbeef-include DIR`), API floor 1.16 (DeaDBeeF >= 1.9.3); `ddb_lxlyrics.so` → `~/.local/lib/deadbeef/`, then restart the player.
- **Rhythmbox** — no build: `lxlyrics.py` + `lxlyrics.plugin` → `~/.local/share/rhythmbox/plugins/lxlyrics/`; needs Rhythmbox with Python plugin support (the libpeas python3 loader), else the plugin cannot be loaded.
- **Audacious** — headers from Audacious ≥ 4.6.1 (`pkg-config audacious`, or `--audacious-include DIR`); `lxlyrics.so` → `<prefix>/lib/audacious/General/` (there is no per-user plugin dir, so this one needs root), then restart Audacious.
- **Quod Libet** — no build: `lxlyrics.py` (the module, not the directory) → `~/.config/quodlibet/plugins/`, restart, and enable it in Music → Plugins.
- **VLC** — VLC 3.0.x module headers (`pkg-config vlc-plugin`, or `--vlc-include DIR`); `liblxlyrics_plugin.so` → a directory on `VLC_PLUGIN_PATH` (default `~/.local/lib/vlc/plugins`, or the scanned system dir when writable), then restart VLC with `--extraintf=lxlyrics`.

Each adapter's README has the full build, install, and test commands.

## License

`lyrics-app/` is Apache-2.0 — its lyric parsing/rendering logic is ported from lx-music-desktop (Apache-2.0) with attribution. The six adapters under `plugins/` (`fooyin/`, `deadbeef/`, `rhythmbox/`, `audacious/`, `quodlibet/`, `vlc/`) are GPL-3.0-only by repo decision, which their hosts permit: Fooyin is GPL-3.0; Rhythmbox, Quod Libet and VLC are GPL-2.0-or-later (whose "or later" clause allows a GPL-3.0 plugin); DeaDBeeF's plugin API header is zlib-licensed; and Audacious' libaudcore is BSD-2-Clause. See `LICENSE` for this repository's terms.
