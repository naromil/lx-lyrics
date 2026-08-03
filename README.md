# lx-music-desktop-lyrics

A standalone desktop lyrics feature extracted from [lx-music-desktop](https://github.com/lyswhut/lx-music-desktop), delivered as two decoupled components: a self-contained lyrics display app and a Fooyin Music Player plugin that feeds it.

## Status

Both components are complete and verified:

- **`lyrics-app/`** — standalone Qt6 / C++23 desktop lyrics window with synchronized scrolling and active-line rendering. Builds clean and passes its full test suite (4 suites, 63 QTest slots). Ready to run on its own (`--demo` or host-driven over the WebSocket protocol).
- **`fooyin-plugin/`** — Fooyin plugin that drives the app: watches playback, reads lyrics (embedded tags + local `.lrc`), and streams track/state/lyrics/spectrum data over a loopback WebSocket. Loads into Fooyin 0.11.1.

## Goal

- **`lyrics-app/`** — standalone Qt6 / C++23 desktop lyrics window with synchronized scrolling and line-level rendering.
- **`fooyin-plugin/`** — Fooyin plugin that watches playback, reads lyrics (embedded tags + local `.lrc`), and streams track/state/lyrics/spectrum data to the app over a loopback WebSocket.

The two components share no source code; `docs/protocol.md` is their only contract.

## Repository layout

| Path | Contents |
|---|---|
| `lyrics-app/` | standalone Qt6 lyrics display — see `lyrics-app/README.md` |
| `fooyin-plugin/` | Fooyin plugin — see `fooyin-plugin/README.md` |
| `docs/` | architecture, protocol, and research summaries |
| `references/` | lx-music-desktop v2.12.2 source (gitignored; read-only reference) |
| `tools/` | development helpers such as a host simulator (planned) |

## Documentation

- `lyrics-app/README.md` — build, run modes, config, tests
- `fooyin-plugin/README.md` — build, install, usage, troubleshooting
- `docs/architecture.md` — component design and decoupling boundary
- `docs/protocol.md` — the WebSocket JSON protocol (the shared contract)
- `docs/research/` — condensed engineering research for the port

## Quick start

```sh
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build        # 1. build the display app
cd ../fooyin-plugin && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build # 2. build the plugin
cp fooyin-plugin/build/fyplugin_lxlyrics.so ~/.local/lib/fooyin/plugins/                        # 3. install, restart Fooyin
```

## License

`lyrics-app/` is Apache-2.0 — its lyric parsing/rendering logic is ported from lx-music-desktop (Apache-2.0) with attribution. `fooyin-plugin/` is GPL-3.0-only because it links Fooyin's GPL-3.0 libraries. See `LICENSE` for this repository's terms.
