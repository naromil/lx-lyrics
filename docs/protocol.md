# Player Feed Protocol (v2)

## 1. Purpose & decoupling boundary

This document is the **only** contract between two independently built components:

- **the player-side adapter** — an in-process extension of a music player (`fooyin-plugin/`, `deadbeef-plugin/`, `rhythmbox-plugin/`, …) that observes playback through the player's native API.
- **`lyrics-app/`** — a standalone, always-on-top desktop lyrics display application (Qt6/C++).

The two sides share **no source code**. Each side is implemented from this document alone.

**Ownership invariant.** The adapter lives *inside the player's process* (it is not a separate process and never a service), and it spawns the display app as its **direct child** (never detached), talking to it over the child's standard streams. The user therefore sees exactly one lyrics-app process, and the lyric window lives and dies with the player process.

**The adapter is the RAW CONTEXT SOURCE only.** It is responsible for exactly two things:

1. **Playing context** — the playing file's path, display metadata, playback state, position, and (when the player exposes one) analyser frames.
2. **Transport** — spawning the child, writing host→app lines to its stdin, reading app→host lines from its stdout.

**The display app owns acquisition, ALL parsing, selection, and rendering.** Specifically, the app:

- Acquires lyrics for the playing file named by `set_info.path`: the same-directory sidecar `<dir>/<completeBaseName>.lrc` first, then the file's embedded lyrics tag — converting non-UTF-8 bytes itself (UTF-8 BOM / UTF-16 BOM / GB18030 / BIG5 via ICU) and reading tags itself (TagLib). The host never converts encodings and never reads lyric files.
- Chooses whether to render `lxlrc` or `lrc` (lxlrc takes priority when present and enabled).
- Builds the extended lyric lines from `tlrc` (translation) and `rlrc` (romaji/romanization), including ordering/swapping of translation vs romaji.
- Decodes `[awlrc:base64,...]` container payloads inside lyric text.
- Performs all karaoke word-tag rendering (`<start,duration>` in ms).
- Recomputes the active line from `played_time`; it does not trust a host-supplied line number (v2 has none).

The adapter must **never** parse LRC, compute line numbers, build extended lyrics, or render anything. It treats lyric strings as opaque UTF-8 text — and, in v2, it usually sends none at all.

---

## 2. Transport

- The host spawns the app as a **direct child process** and wires the protocol to the child's standard streams:
  - child **stdin** — host→app JSON lines. **MUST be a pipe** the adapter writes to;
  - child **stdout** — app→host JSON lines. **MUST be a pipe** the adapter reads from;
  - child **stderr** — free-form logs, never protocol data. The adapter MAY leave it inherited from the player (Rhythmbox and DeaDBeeF do, so the app's logs land in the player's own log) instead of creating a third pipe; if it *does* pipe stderr it **MUST** drain that pipe, because a full stderr pipe blocks the app's logger and with it the whole session.
- **One JSON object per UTF-8 line**, terminated by `\n`. A raw newline inside a string is invalid — JSON escaping (`\n`) is required, otherwise the message would be split in two.
- **The app's stdout carries protocol lines only.** Every log the app emits goes to stderr, so an adapter can parse stdout unconditionally.
- A line longer than **1 MiB** is a protocol error (§7).
- **EOF on the app's stdin means the player is gone: the app quits immediately**, with no close animation. Closing the child's stdin is therefore the adapter's normal way to end a session; the app is never left running after its player exits.
- **A closed or stalled app stdout is not a quit trigger.** The app's app→host writes are non-blocking and bounded: if the host stops draining the child's stdout (or closes it) while the child's stdin stays open, the app logs **one** warning, drops its further app→host lines and keeps running. Only EOF on stdin ends a session.
- The adapter does **not** wait for the app to ask for anything before pushing state: the initial snapshot follows the handshake immediately (§5), so pipe buffering makes the spawn race-free — there is no request/response round trip to bootstrap.

---

## 3. App CLI contract

The display app accepts the following command-line arguments:

| Flag | Meaning |
|------|---------|
| `--player-feed` | Host-driven mode: speak this protocol over stdin/stdout. The app is a direct child of the player's adapter; EOF on stdin terminates it. |
| `--demo` | Standalone/demo mode: no host. The app self-feeds a fake track, lyric and spectrum data and runs its full parse/render pipeline against it. Used for development and screenshots. |

With no flag the app opens an inert window (no host, no self-feed).

`--player-feed` wins over `--demo` when both are given.

The adapter spawns the app with an **argv array** — `[app_path, "--player-feed"]` — never through a shell (§8). Nothing else is passed: the playing context arrives as `set_info`/`set_status` lines (§5) as soon as the child is up.

---

## 4. Message actions (app → host)

Every message carries a `v` field (§9). These two actions are the **complete** app→host vocabulary in v2.

### `get_analyser_data_array`

Request one spectrum snapshot; the host answers with a `spectrum` line (§5). Sent **only after** the host declared `"spectrum":true` in its hello line — an app that keeps asking a host without an analyser is a broken app, and a host that declared no analyser MUST never receive this request.

```json
{"v": 2, "action": "get_analyser_data_array"}
```

### `close_requested`

The user **intentionally closed** the desktop lyric window (control-bar close button or WM close/Alt+F4). No payload.

The host ends its display session — unchecks its "Desktop Lyrics" toggle and stops its spawner — so the exit that follows is **never** treated as a crash and the app is **not** respawned. The handler is idempotent: a duplicate line or a toggle already off is a no-op.

The app reports only *this* close: when the **player** ends the session (stdin EOF, §2), the app quits silently and MUST NOT send `close_requested` — a player-initiated shutdown must never look like the user turning the feature off.

```json
{"v": 2, "action": "close_requested"}
```

---

## 5. Message actions (host → app)

All host→app messages are JSON lines. The **first** one MUST be `hello` (§7).

| action | Payload fields | Trigger / semantics |
|--------|----------------|---------------------|
| `hello` | `host, spectrum` | **First line, always.** `host` is a display/log name for the adapter (`"fooyin"`, `"deadbeef"`, `"rhythmbox"`); `spectrum` (boolean) declares whether the host can answer analyser requests. |
| `set_info` | `path, singer, name, album, lrc, tlrc, rlrc, lxlrc, isPlay, played_time` | Full snapshot. Sent right after the handshake, on track change, and after metadata edits. `played_time` is in **ms**. |
| `set_lyric` | `lrc, tlrc, rlrc, lxlrc` | Lyric-only update, no metadata/state. Sent when the host has lyric text of its own (an edit, an external source) while the same track plays. |
| `set_status` | `isPlay, played_time` | Playback state sync (start/pause/seek/finish). Periodic while playing; the app recomputes the active line from `played_time`. |
| `set_offset` | `tempOffset` | Lyric time offset in **ms**. Semantics mirror lx-music: `tempOffset` is a **delta** from the lyric's own offset tag (`[offset:…]`), not an absolute offset. |
| `set_playbackRate` | `rate` | Float playback rate. **Supported range: `0.25`–`4.0`** (inclusive). Rates outside the range — including `0`, negatives, `NaN` and infinities — are **ignored by the app** (the current rate is kept), so hosts should clamp before sending. **Reserved for hosts** that expose rate control. |
| `set_play` | `time` | Resume/seek to `time` (ms). App resumes/restarts playback of the lyric timer at that position. |
| `set_pause` | *(none)* | Pause lyric rendering. |
| `set_stop` | *(none)* | Stop playback entirely; clear the active lyric line. |
| `open_settings` | *(none)* | Requests the lyrics app to open its configuration dialog (equivalent to the app's settings gear / Ctrl+,). Lets the user reconfigure a locked lyrics window. |
| `set_fullscreen` | `isFullscreen` | The host's main window entered/left fullscreen. When `desktopLyric.fullscreenHide` is enabled the app hides the lyric window while `isFullscreen` is true and shows it again when false. Mirrors lx-music's `main_window_fullscreen` event. |
| `spectrum` | `data` | Spectrum data: `{"v":2,"action":"spectrum","data":"<base64 of exactly 128 bytes>"}` — the reply to `get_analyser_data_array`. A wrong-sized payload is dropped loudly (never fatal). See the conversion contract below. |

### `set_info`: how the app gets lyrics

v2 removed the v1 `id` and `line` fields: no host computes line numbers any more (the app recomputes the active line from `played_time`), and nothing consumed the opaque track id. `path` is new — it is the local filesystem path of the playing file (`""` when unknown, e.g. streams and CUE tracks).

**Lyric precedence:**

1. A **non-empty `lrc`** from the host wins — the host had lyric text of its own and the app renders it as-is (still splitting `tlrc`/`rlrc`/`lxlrc` out of it normally).
2. Otherwise (empty or omitted `lrc`) the app **reads the file at `path`**: the `<dir>/<completeBaseName>.lrc` sidecar first, then the file's embedded lyrics tag, joined with `\n` when both exist. No path or no lyrics ⇒ empty lyrics, and the window shows its no-lyrics state.

`tlrc`, `rlrc` and `lxlrc` are only ever host-supplied text: the app does not derive them from the file.

### `spectrum` conversion contract

The host computes the 128 bytes from the player's float spectrum magnitudes:

```
byte = clamp(round(255 * log10(1 + magnitude * SCALE) / log10(256)), 0, 255)
```

- `magnitude` — the player's normalized float magnitude for a frequency bin (nominally `0.0–1.0`; may transiently exceed `1.0` and is clamped).
- `SCALE` — a documented constant: **`255.0`**. It is chosen so that a full-scale magnitude of `1.0` maps to exactly `255`: `255 * log10(1 + 255) / log10(256) = 255 * log10(256) / log10(256) = 255`. Larger magnitudes clamp to `255`; zero maps to `0`.
- The host downsamples the player's frequency bins to exactly **128** output bins (e.g., evenly spaced aggregation across the audible spectrum, host-defined).
- The payload is exactly **128 bytes**, each byte `0–255` log-scaled, produced by the host from its float spectrum. Byte ordering/indexing semantics (e.g., how the app maps bytes to bars) are app-internal rendering details and are **not** specified by this protocol.

---

## 6. Data formats

| Field | Format |
|-------|--------|
| `path` | Local filesystem path of the playing file. `""` when unknown. |
| `lrc`, `tlrc`, `rlrc` | LRC text (UTF-8). May be the empty string `""` when absent. A host may send the concatenation of multiple lyric sources in `lrc` (e.g. embedded tag + sidecar file); the app merges duplicate timestamps into extended lyric lines and suppresses exact-text duplicates. |
| `lxlrc` | LX karaoke format; word tags are `<start,duration>` with both values in **ms**. May be `""` when absent. |
| `singer`, `name`, `album` | Display strings. May be `""` when absent. |
| `host` | Adapter display/log name (e.g. `"fooyin"`). |
| `isPlay` | Boolean playback state. |
| `spectrum` | Boolean; the host can answer analyser requests. |
| `isFullscreen` | Boolean; the host's main window is fullscreen. |
| `played_time` | Integer milliseconds. |
| `tempOffset` | Integer milliseconds (delta from the lyric's own offset tag). |
| `rate` | Float. Must be finite and within `0.25`–`4.0` (inclusive); the app ignores out-of-range or non-finite values and keeps the current rate. |
| `time` | Integer milliseconds. |
| `data` | Base64 of exactly 128 bytes of log-scaled spectrum magnitudes (§5). |

All strings are **UTF-8**. The host sends text it already holds as UTF-8; the app performs every encoding conversion for the files it reads itself.

**Missing vs. wrong-typed fields.** A field that is **omitted** (or JSON `null`) takes its empty/default value and is not an error — an adapter may omit what it does not know (`path` for a stream, the lyric keys entirely). A field that is **present with the wrong type** (a number where a string belongs, `"yes"` for a boolean, …) is a protocol error (§7).

---

## 7. Error handling (fail fast, fail loud)

- **Malformed JSON**, a **missing/unsupported `v`**, an **unknown action**, a **message before `hello`**, a **wrong-typed field**, or a **line over 1 MiB** is a protocol error on the receiving side.
- **Skipping vs. failing.** A line that is **not a JSON object** MAY be skipped with a loud log: §2 guarantees the app writes nothing but protocol lines to stdout, so a non-JSON line is a library leaking into the stream, not the peer misbehaving (an implementation that treats it as a protocol error anyway is stricter, and equally conforming). Everything the first rule lists — a missing/unsupported `v`, an unknown action, a malformed payload, a message before `hello`, the 1 MiB cap — is a protocol error on whichever side receives it: log it loudly, end the session, never respawn the child.
- On the app side a protocol error is **fatal**: log it loudly on stderr and **exit non-zero**. The app must never half-render a session it cannot trust — a host that speaks a different protocol gets an obviously dead child, not a window showing something plausible.
- On the adapter side a protocol error means the child is misbehaving: log it loudly, end the session (do not respawn a child that violated the protocol), and surface the failure to the user by turning the toggle off. The only input an adapter may tolerate is the non-JSON line above; a line that speaks the protocol but breaks it is never repaired or ignored.
- Both sides parse **strictly at the pipe boundary** into typed message structs — a **single parse point** per side, versioned and action-checked. Everything downstream operates on typed values only.
- A protocol error ends the session; there is no reconnect. A new session is a new child process (spawned by the player adapter when the user re-enables the feature).

---

## 8. Security

- **No listening ports, no sockets, no loopback.** The transport is a private parent→child pipe pair created by the player's own `spawn`; nothing outside the parent and its child can read or write a session.
- The app is spawned as a **direct child**, which is also what makes the lifetime guarantee (§2) enforceable.
- Argument lists are passed as an **argv array** — **no shell interpolation**. User-supplied strings (paths, names) are never assembled into a shell command line.
- No secrets, tokens, or credentials are ever transmitted.
- The app reads files only at `set_info.path` (and the sidecar next to it); it opens no network connections of its own.

---

## 9. Versioning

- Every JSON message carries a protocol version field: **`"v": 2`** (integer). There are no binary frames in v2 — spectrum data is base64 inside a JSON line.
- The first host→app line is the handshake (§5); a message with an unsupported `v`, or any message arriving before `hello`, is a protocol error (§7) — never a partially accepted session.
- **This document is the single source of truth.** A future protocol change that is not backward-compatible bumps `v`; each side implements exactly one version per session.
- v2 changed the transport (pipes instead of WebSocket), added `hello`/`set_info.path`, removed `id`/`line`, and removed the `get_info`/`get_status` app requests (the host pushes state instead of being polled).

---

## 10. Example session

```
spawn:    player adapter → child     [lx-lyrics-app, --player-feed]     (stdin/stdout pipes, not detached)

handshake: host → app   {"v":2,"action":"hello","host":"fooyin","spectrum":true}
snapshot:  host → app   {"v":2,"action":"set_info","path":"/music/song.mp3","singer":"Singer",
                         "name":"Song","album":"Album","lrc":"","tlrc":"","rlrc":"","lxlrc":"",
                         "isPlay":true,"played_time":0}
                         # app reads /music/song.mp3's sidecar/embedded lyrics itself

playback:  host → app   {"v":2,"action":"set_play","time":0}
sync:      host → app   {"v":2,"action":"set_status","isPlay":true,"played_time":15450}
seek:      host → app   {"v":2,"action":"set_play","time":42000}
pause:     host → app   {"v":2,"action":"set_pause"}
lyric edit:host → app   {"v":2,"action":"set_lyric","lrc":"[00:12.34]line","tlrc":"","rlrc":"","lxlrc":""}

spectrum:
req bars:  app  → host  {"v":2,"action":"get_analyser_data_array"}
bars:      host → app   {"v":2,"action":"spectrum","data":"<base64, exactly 128 bytes>"}

open settings (from the player's LX Lyrics settings page):
           host → app   {"v":2,"action":"open_settings"}      # app opens/raises its config dialog

user close (control-bar X or WM close):
           app  → host  {"v":2,"action":"close_requested"}    # host stops its session; no respawn
           app exits after its close animation

player shutdown:
           host closes the child's stdin (EOF) → app exits immediately, silently
           (the window's shutdown teardown is NOT a user close: no close_requested)
```

---

## 11. Future extensions

- **`tlrc` / `rlrc` / `lxlrc` are carried but normally empty.** v2 adapters send playing context (path, metadata, state, position) and let the app acquire lyrics; a host with lyric text of its own uses `set_lyric`/`set_info.lrc`, and the wire format already supports the richer fields so no protocol change is needed to enable them.
- **`set_playbackRate` is reserved.** Adapters of players without a rate API send nothing; the field exists so rate-capable players can use it. The app only accepts finite rates in `0.25`–`4.0` (§5/§6); anything else is ignored and the current rate is kept.
- **`open_settings` is a control message, not a state message.** It carries no payload and expects no reply; the app opens its configuration dialog, or raises an already-open one.
- **`close_requested` is a control message, not a state message.** It carries no payload and expects no reply; the host ends its session without respawning (§4). It describes a *user* action only — see §4 for the EOF case.
- **Spectrum availability is per-host, not per-protocol.** `hello.spectrum` is how a player without an analyser (Rhythmbox, for example) declares that there will be no frames; the app then never asks, and the visualizer simply stays idle.
- **Wayland always-on-top caveat is app-level, not protocol.** Making the lyric window stay on top under Wayland compositors is entirely the display app's concern and out of scope for this wire contract.
