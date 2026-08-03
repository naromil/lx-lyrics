# Desktop Lyrics Bridge Protocol (v1)

## 1. Purpose & decoupling boundary

This document is the **only** contract between two independently built components:

- **`fooyin-plugin/`** — a plugin for the Fooyin Music Player (Qt6/C++).
- **`lyrics-app/`** — a standalone, always-on-top desktop lyrics display application (Qt6/C++).

The two sides share **no source code**. Each side is implemented from this document alone.

**The host plugin is the RAW DATA SOURCE only.** It is responsible for exactly two things:

1. **Encoding conversion** — any non-UTF-8 lyric text it reads from embedded tags or local files is converted to UTF-8 (via ICU) before being sent.
2. **Transport** — opening the WebSocket server, accepting the display app, and shipping messages/frames as defined below.

**The display app owns ALL parsing, selection, and rendering.** Specifically, the app:

- Chooses whether to render `lxlrc` or `lrc` (lxlrc takes priority when present and enabled).
- Builds the extended lyric lines from `tlrc` (translation) and `rlrc` (romaji/romanization), including ordering/swapping of translation vs romaji.
- Decodes `[awlrc:base64,...]` container payloads inside lyric text.
- Performs all karaoke word-tag rendering (`<start,duration>` in ms).
- Recomputes the active line from `played_time`; it does not trust a host-supplied line number.

The plugin must **never** parse LRC, compute line numbers, build extended lyrics, or render anything. It treats lyric strings as opaque UTF-8 text.

---

## 2. Transport

- **WebSocket over loopback TCP**, bound to `127.0.0.1` only.
- The host runs a `QWebSocketServer` on an **ephemeral port** (port 0 → OS-assigned), and the display app connects as a **client** to the URL the host provides (see §3).
- **Text frames** carry all JSON messages.
- **One binary frame type** exists: `send_analyser_data_array` (spectrum data, see §5).
- **1:1 connection model:**
  - If the app disconnects, the host may respawn/restart its lyric display session (or simply wait for a reconnect) — the host keeps serving on the same port.
  - If the host socket closes, an app started with `--exit-on-disconnect` terminates itself.

---

## 3. App CLI contract

The display app accepts the following command-line arguments:

| Flag | Meaning |
|------|---------|
| `--ws=ws://127.0.0.1:PORT` | The host's WebSocket URL to connect to (default: `ws://127.0.0.1:0` is invalid — must be supplied when connecting to a host). |
| `--exit-on-disconnect` | Quit the app when the connection closes. Used when the app is spawned as a child of the host plugin, so the lyric window lives and dies with the connection. |
| `--demo` | Standalone/demo mode: no host. The app self-feeds fake track, lyric, and playback-state data and runs its full parse/render pipeline against it. Used for development and screenshot/testing. |

The plugin spawns the app with `--ws=ws://127.0.0.1:PORT --exit-on-disconnect` (plus any window/geometry args the app defines).

---

## 4. Message actions (app → host)

All app→host messages are JSON text frames. Every message carries a `v` field (see §9).

### `get_info`

Request the full current track snapshot. Host replies with `set_info` (§5).

```json
{"v": 1, "action": "get_info"}
```

### `get_status`

Request the current playback state. Host replies with `set_status` (§5).

```json
{"v": 1, "action": "get_status"}
```

### `get_analyser_data_array`

Request one spectrum snapshot. Host replies with a **binary** `send_analyser_data_array` frame (§5).

```json
{"v": 1, "action": "get_analyser_data_array"}
```

---

## 5. Message actions (host → app)

All host→app messages are JSON text frames **except** `send_analyser_data_array`, which is a binary frame. Every JSON message carries a `v` field (see §9).

| action | Payload fields | Trigger / semantics |
|--------|----------------|---------------------|
| `set_info` | `id, singer, name, album, lrc, tlrc, rlrc, lxlrc, isPlay, line, played_time` | Full snapshot. Sent on track change and in reply to `get_info`. `played_time` is in **ms**. |
| `set_lyric` | `lrc, tlrc, rlrc, lxlrc` | Lyric-only update, no metadata/state. Sent on lyric refetch or lyric edit while the same track is playing. |
| `set_status` | `isPlay, line, played_time` | Playback state sync (start/pause/seek/finish). `line` **SHOULD be `-1`** — hosts that do not parse LRC cannot compute a line number; the app recomputes the line from `played_time` and ignores `line` entirely. |
| `set_offset` | `tempOffset` | Lyric time offset in **ms**. Semantics mirror lx-music: `tempOffset` is a **delta** from the lyric's own offset tag (`[offset:…]`), not an absolute offset. |
| `set_playbackRate` | `rate` | Float playback rate, e.g. `0.5`, `1.0`, `2.0`. Fooyin sends `1.0` (Fooyin exposes no rate API). **Reserved for future hosts** that support rate control. |
| `set_play` | `time` | Resume/seek to `time` (ms). App resumes/restarts playback of the lyric timer at that position. |
| `set_pause` | *(none)* | Pause lyric rendering. |
| `set_stop` | *(none)* | Stop playback entirely; clear the active lyric line. |
| `send_analyser_data_array` | *(binary frame)* | Spectrum data. **Exactly 128 bytes**, each byte 0–255, log-scaled spectrum magnitudes. See conversion contract below. |

### `send_analyser_data_array` conversion contract

The host computes the 128 bytes from Fooyin's float spectrum magnitudes:

```
byte = clamp(round(255 * log10(1 + magnitude * SCALE) / log10(256)), 0, 255)
```

- `magnitude` — Fooyin's normalized float magnitude for a frequency bin (nominally `0.0–1.0`; may transiently exceed `1.0` and is clamped).
- `SCALE` — a documented constant: **`255.0`**. It is chosen so that a full-scale magnitude of `1.0` maps to exactly `255`: `255 * log10(1 + 255) / log10(256) = 255 * log10(256) / log10(256) = 255`. Larger magnitudes clamp to `255`; zero maps to `0`.
- The host downsamples Fooyin's frequency bins to exactly **128** output bins (e.g., evenly spaced aggregation across the audible spectrum, host-defined).
- The frame is exactly **128 bytes**, each byte `0–255` log-scaled, produced by the host from its float spectrum. Byte ordering/indexing semantics (e.g., how the app maps bytes to bars) are app-internal rendering details and are **not** specified by this protocol.

---

## 6. Data formats

| Field | Format |
|-------|--------|
| `lrc`, `tlrc`, `rlrc` | LRC text (UTF-8). May be the empty string `""` when absent. |
| `lxlrc` | LX karaoke format; word tags are `<start,duration>` with both values in **ms**. May be `""` when absent. |
| `id` | Opaque track id string. May be `""`/`null` for "no track". |
| `singer`, `name`, `album` | Display strings. May be `""` when absent. |
| `isPlay` | Boolean playback state. |
| `line` | Integer; `-1` unless the host actually parses LRC (see §5 — the app ignores it). |
| `played_time` | Integer milliseconds. |
| `tempOffset` | Integer milliseconds (delta from the lyric's own offset tag). |
| `rate` | Float. |
| `time` | Integer milliseconds. |

All strings are **UTF-8**. The host performs all encoding conversion (non-UTF-8 → UTF-8 via ICU) at the boundary; the app never receives raw non-UTF-8 text.

---

## 7. Error handling (fail fast, fail loud)

- **Malformed JSON** or **unknown `action`** on the receiving side → the receiving side **logs the error and closes the connection**. It must **never half-render** partial data.
- Both sides parse **strictly** at the socket boundary into typed message structs — a **single parse point** per side. Everything downstream operates on typed values only.
- After a close caused by an error, the host may accept a new connection; an app spawned with `--exit-on-disconnect` terminates.

---

## 8. Security

- The host binds `127.0.0.1` **only** — no remote access, no `0.0.0.0`.
- The host accepts **exactly one** client. If a second client connects while one is already connected, the host **rejects and closes** the new connection.
- No secrets, tokens, or credentials are ever transmitted.
- When the host spawns the app via `QProcess`, argument lists are passed as an argv array — **no shell interpolation**; user-supplied strings (paths, ids) must never be assembled into a shell command line.

---

## 9. Versioning

- Every JSON message SHOULD carry a protocol version field: **`"v": 1`** (integer). Binary frames carry no version; their shape is fixed by the action that requests them.
- **This document is the single source of truth.** A future protocol change that is not backward-compatible bumps `v`; a receiving side that sees an unsupported `v` closes the connection per §7.

---

## 10. Example session

```
connect:  app  → host    ws://127.0.0.1:49812
req info: app  → host    {"v":1,"action":"get_info"}
snapshot: host → app     {"v":1,"action":"set_info","id":"7f3a…","singer":"Singer","name":"Song","album":"Album",
                          "lrc":"[00:12.34]first line\n[00:15.67]second line","tlrc":"","rlrc":"","lxlrc":"",
                          "isPlay":true,"line":-1,"played_time":12340}

playback sync (periodic, e.g. on state/seek events):
          host → app     {"v":1,"action":"set_status","isPlay":true,"line":-1,"played_time":15450}
          host → app     {"v":1,"action":"set_play","time":42000}          # user seeked
          host → app     {"v":1,"action":"set_status","isPlay":false,"line":-1,"played_time":44900}

spectrum:
req bars: app  → host    {"v":1,"action":"get_analyser_data_array"}
bars:     host → app     <binary frame, exactly 128 bytes>

close:    host socket closes → app started with --exit-on-disconnect terminates.
```

---

## 11. Future extensions

- **`tlrc` / `rlrc` / `lxlrc` are carried but empty in v1 from the Fooyin plugin.** v1 lyric sources are embedded tags and local `.lrc` files only. The wire format already supports them (§5/§6) so richer sources can be enabled without a protocol change.
- **`set_playbackRate` is reserved.** Fooyin sends `1.0`; the field exists so rate-capable hosts can use it later.
- **Wayland always-on-top caveat is app-level, not protocol.** Making the lyric window stay on top under Wayland compositors is entirely the display app's concern and out of scope for this wire contract.
