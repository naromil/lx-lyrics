# Architecture

## Overview

The desktop-lyrics feature is extracted from lx-music-desktop into two independent components that communicate only through a WebSocket JSON protocol:

- **`lyrics-app/`** — a standalone Qt6 / C++23 desktop lyrics display with karaoke rendering (the port of lx-music's `renderer-lyric` + `lyric-font-player`).
- **`fooyin-plugin/`** — a Fooyin Music Player plugin that acts as a raw data source for the app.

The two are separate CMake projects with zero shared source. The app does not know about Fooyin; the plugin contains no display code. `docs/protocol.md` is the only shared contract.

## Components

### lyrics-app

- Owns **all parsing**: LRC, translation (`tlyric`), romaji (`rlyric`), word-level (`lxlyric`), and the `[awlrc:…]` container.
- Owns **all selection logic**: lxlrc-vs-lrc choice, extended-lyrics construction (translation/romaji swap), offset handling.
- Owns **all rendering**: synchronized scrolling, word-level karaoke fill, colors/fonts/opacity, vertical and horizontal layouts, spectrum visualization, and window management (frameless, always-on-top, lock, hover-hide, etc.).
- Owns the full settings surface ported from `desktopLyric.*`, persisted in its own config store.
- Connects to the plugin over loopback WebSocket; also runs a `--demo` mode so it can be developed and tested without Fooyin.

### fooyin-plugin

- Provides the plugin shell (`Plugin + CorePlugin + GuiPlugin`), a menu action, and a settings page.
- Watches `PlayerController` for track metadata, play state, and position; reads embedded lyrics via `Track::extraTag()` and local `.lrc` sidecar files; samples the analyser via `EngineController::visualisationService()`.
- Spawns `lyrics-app` and feeds it a JSON state stream (track info, position, state, lyrics, spectrum) over the WebSocket.
- Contains **no** lyric parsing or display logic — it passes raw data through the protocol.

## Decoupling boundary

The boundary is intentional and strict:

- Plugin = raw data acquisition + encoding + transport.
- App = parsing, selection, and rendering.

Pushing selection logic (e.g. lxlrc-vs-lrc) or container decoding into the plugin would couple the plugin to lx-music display semantics. All such decisions stay in the app, driven by the app's own settings.

## The protocol

`docs/protocol.md` defines the message action set (mirroring lx-music's MessagePort actions), the JSON payload schemas, the binary spectrum frame format, and the CLI contract (`--ws`, `--exit-on-disconnect`, `--demo`). The app parses strictly at the socket boundary; unknown or malformed frames fail loudly rather than half-rendering.

## Key decisions

- **Native C++/Qt6** for the app (no Electron) — the extracted feature is rewritten as a Qt widget app.
- **Full settings port** — the `desktopLyric.*` key set and defaults are carried over verbatim.
- **v1 lyric sources: tags + local `.lrc`** — embedded-tag lyrics and sidecar files only; online lyric APIs are deferred past v1.
- **Spectrum in v1** — the analyser stream is part of the v1 protocol; the float→byte conversion is defined in `docs/protocol.md`.
- **Separate app + socket** instead of a Fooyin-embedded window — keeps the display host-agnostic and testable, at the cost of a small process and loopback overhead.

## Status

Active extraction from lx-music-desktop (Apache-2.0) with attribution. The ported app logic keeps Apache-2.0; the plugin links GPL-3.0 Fooyin libraries and is licensed GPL-3.0. Research reports live in `docs/research/`.
