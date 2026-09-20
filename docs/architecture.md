# Architecture

## Overview

The desktop-lyrics feature is extracted from lx-music-desktop into four independent projects with **zero shared C++ source**; the only contract between them is `docs/protocol.md` **v2** — a stdin/stdout player feed:

- **`lyrics-app/`** — a standalone Qt6 / C++23 desktop lyrics display with karaoke rendering (the port of lx-music's `renderer-lyric` + `lyric-font-player`). It owns lyric acquisition, parsing, selection, and rendering.
- **`fooyin-plugin/`**, **`deadbeef-plugin/`**, **`rhythmbox-plugin/`** — in-process adapters for their players. Each observes playback through the player's native API and supplies playing context to the app.

The app does not know which player drives it; an adapter contains no acquisition, parsing, or display code.

## Ownership and lifetime

The adapter lives **inside the player's process** (a Fooyin Qt plugin, a DeaDBeeF `DB_misc_t` module, a Rhythmbox libpeas plugin — never a separate process and never a service) and spawns `lx-lyrics-app --player-feed` as its **direct child** (never detached), with the child's stdin and stdout as the protocol pipes: stdin carries host→app JSON lines, stdout carries app→host JSON lines. Stderr carries only free-form logs and an adapter may leave it inherited from the player rather than piping it (Rhythmbox and DeaDBeeF do; a piped stderr would have to be drained, since a full pipe blocks the app's logger).

Consequences:

- The user sees exactly one lyrics-app process, and the lyric window lives and dies with the player: closing the child's stdin (player exit, toggle off) makes the app quit immediately.
- `close_requested` is the one app→host message that is not a request for data — the user closed the lyric window, so the adapter ends its session and does not respawn.
- Nothing listens on a port; the pipe pair is private to the parent and its child.

## Components

### lyrics-app

- Owns **acquisition**: for the file at `set_info.path`, the same-directory `<dir>/<completeBaseName>.lrc` sidecar and the embedded lyrics tag (TagLib) — joined when both exist — with encoding conversion (UTF-8 BOM / UTF-16 BOM / GB18030 / BIG5 via ICU).
- Owns **all parsing**: LRC, translation (`tlrc`), romaji (`rlrc`), word-level (`lxlrc`), and the `[awlrc:…]` container.
- Owns **all selection logic**: lxlrc-vs-lrc choice, extended-lyrics construction (translation/romaji swap), offset handling, and active-line recomputation from `played_time`.
- Owns **all rendering**: synchronized scrolling, word-level karaoke fill, colors/fonts/opacity, vertical and horizontal layouts, spectrum visualization, and window management (frameless, always-on-top, lock, hover-hide, etc.).
- Owns the full settings surface ported from `desktopLyric.*`, persisted in its own config store.
- Run modes: `--player-feed` (host-driven), `--demo` (self-fed fake track + synthetic spectrum), or an inert window when no flag is given.

### fooyin-plugin / deadbeef-plugin / rhythmbox-plugin

Each adapter:

- Provides the plugin shell for its player (Fooyin `Plugin`/`CorePlugin`/`GuiPlugin`, DeaDBeeF `DB_misc_t`, Rhythmbox `Peas.Activatable`), a session toggle, and — for Fooyin and Rhythmbox — a settings/preferences pane.
- Watches its player's playback API (Fooyin `PlayerController`, DeaDBeeF events + `streamer_get_playing_track_safe`, Rhythmbox `RBShellPlayer` signals) and pushes `set_info`/`set_status`/`set_play`/`set_pause`/`set_stop` lines carrying the file path, metadata, state, and position.
- Spawns the app as its direct child and strictly parses the two app→host actions (`get_analyser_data_array`, `close_requested`).
- Samples the player's analyser where one exists (Fooyin `VisualisationService`, DeaDBeeF `vis_spectrum_listen2`) and answers `spectrum` requests; Rhythmbox has no analyser API and declares `spectrum: false`.
- Contains **no** acquisition, lyric parsing, or display logic — it treats lyric strings as opaque and normally sends none at all.

## Decoupling boundary

The boundary is intentional and strict:

- **Adapter = playing context + transport.** It never reads lyric files, never converts encodings, never parses LRC, and never computes line numbers.
- **App = acquisition, parsing, selection, and rendering**, driven by its own settings.

Pushing selection logic (e.g. lxlrc-vs-lrc) or container decoding into an adapter would couple it to lx-music display semantics. All such decisions stay in the app.

## The protocol

`docs/protocol.md` defines the v2 contract: the `hello` handshake, the host→app action set (`set_info`, `set_lyric`, `set_status`, `set_offset`, `set_playbackRate`, `set_play`, `set_pause`, `set_stop`, `set_fullscreen`, `open_settings`, `spectrum`), the two app→host actions (`get_analyser_data_array`, `close_requested`), the JSON payload schemas, the 128-byte log-scaled spectrum format, the CLI contract (`--player-feed`, `--demo`), and the strict fail-fast error rules. Both sides parse strictly at the pipe boundary; a protocol error ends the session.

## Key decisions

- **Native C++/Qt6** for the app (no Electron) — the extracted feature is rewritten as a Qt widget app.
- **Player-owned adapters, app-owned acquisition** — the adapter lives in the player process and spawns the app as its child, so the window shares the player's lifetime; the app reads the lyrics itself, so every adapter stays thin and the acquisition logic exists exactly once.
- **Full settings port** — the `desktopLyric.*` key set is carried over verbatim, but the default values are tuned to user preference (enable, isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width, fontSize, opacity, isZoomActiveLrc deviate from upstream); window positions default to auto (null).
- **User-tuned behavior deviations** — the close button fades the content out (300 ms, reusing the reference's `#container` opacity-transition idiom) before quitting, where the reference closes the window instantly; and the active lyric line renders at the full configured played color, where the reference's hardcoded `body { opacity: .8 }` dims every pixel to 80%. Non-active lines keep that dimming here (applied per-line as `(opacity/100) × lerp(0.8, 1.0, colorProgress)` instead of a window-wide effect); all other quit paths (stdin EOF / player shutdown, WM close) stay instant like the reference.
- **Lyric sources: tags + local `.lrc`** — embedded-tag lyrics and sidecar files only; online lyric APIs are deferred.
- **Spectrum is optional per host** — the analyser stream is part of the protocol, the float→byte conversion is defined in `docs/protocol.md`, and a host without an analyser declares `spectrum: false` (the app then never asks).
- **Separate app process + pipes** instead of an embedded window — keeps the display host-agnostic and testable, at the cost of a small process.

## Status

Active extraction from lx-music-desktop (Apache-2.0) with attribution. The ported app logic keeps Apache-2.0; the adapters are GPL-3.0-only, which their hosts permit (Fooyin is GPL-3.0; Rhythmbox is GPL-2.0-or-later, whose "or later" clause allows a GPL-3.0 plugin; DeaDBeeF's plugin API header is zlib-licensed). Research reports live in `docs/research/`.
