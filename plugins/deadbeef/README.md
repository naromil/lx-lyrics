# lx-lyrics DeaDBeeF adapter

A [DeaDBeeF](https://deadbeef.sourceforge.net/) plugin (`ddb_lxlyrics.so`) that owns the
desktop-lyrics session: it spawns the standalone **lx-lyrics** display as its direct child and
feeds it the player state over the **protocol v2 stdin/stdout player feed**
(`../../docs/protocol.md`). The plugin never reads lyrics — the app owns acquisition (sidecar `.lrc`
+ embedded tags), parsing and rendering.

Closing the lyric window sends `close_requested`; the adapter ends the session, the app exits on
stdin EOF, and the toggle clears itself.

## Requirements

- DeaDBeeF with **API ≥ 1.16**, i.e. **deadbeef ≥ 1.9.3**. Two upstream facts pin this:
  - `streamer_get_playing_track_safe()` is only declared under `#if (DDB_API_LEVEL >= 16)`
    (`include/deadbeef/deadbeef.h:1761-1766` in 1.10.1 and master alike), so compiling at 15 fails
    outright — GCC: `'DB_functions_t' has no member named 'streamer_get_playing_track_safe'`.
    The declared floor has to match the level the plugin compiles with: the field sits past the end
    of a 1.15 host's shorter `DB_functions_t`, and `src/plugins.c:780` refuses to load a plugin
    whose `api_vminor` is above the host's (`WARNING: plugin "…" wants API v1.16 (got 1.15), will
    not be loaded`). So 1.9.0–1.9.2 (API 1.15) can only be supported by switching the two call
    sites in `src/plugin.c` to the deprecated `streamer_get_playing_track()` and lowering the level
    to 15 — that trades upstream's streamer lock for these releases.
  - The analyser needs 1.15 (`vis_spectrum_listen2`); hosts without it still work, with
    `hello.spectrum: false`.
- Deadbeef headers, from a source checkout or an installed SDK:
  ```sh
  git clone --depth 1 --branch 1.10.1 https://github.com/DeaDBeeF-Player/deadbeef
  ```
  (its `include/deadbeef/deadbeef.h` is API 1.19 and contains everything 1.16 needs).
- `lx-lyrics-app` installed (e.g. by `../../tools/install.sh`), findable on `$PATH`.

## Build

CMake (headers via `-DDEADBEEF_INCLUDE_DIR`):

```sh
cmake -B build -G Ninja -DDEADBEEF_INCLUDE_DIR=/path/to/deadbeef/include
cmake --build build          # -> build/ddb_lxlyrics.so
ctest --test-dir build       # hermetic protocol tests, no DeaDBeeF needed
```

A bare `cmake` configures **Debug** (the same default the app project uses); pass
`-DCMAKE_BUILD_TYPE=Release` for the plugin you install.

Or directly, with the same four sources:

```sh
cc -std=c99 -fPIC -shared -O2 -I/path/to/deadbeef/include \
   -o ddb_lxlyrics.so src/plugin.c src/feed.c src/json.c src/spectrum.c -lpthread -lm
```

`src/plugin.c` and `src/spectrum.c` pin `#define DDB_API_LEVEL 16` before including the header, so
using newer API is a compile error rather than a silent dependency; pass no `-DDDB_API_LEVEL`.

The module name matters: DeaDBeeF derives the entry symbol from the file basename
(`src/plugins.c`), so the file must be `ddb_lxlyrics.so` and it exports
`DB_plugin_t *ddb_lxlyrics_load(DB_functions_t *)`.

## Install

```sh
mkdir -p ~/.local/lib/deadbeef
install -m 755 build/ddb_lxlyrics.so ~/.local/lib/deadbeef/
```

DeaDBeeF searches, in order: `~/.local/lib64/deadbeef`, `~/.local/lib/deadbeef`, then its system
plugin directory; with `XDG_LOCAL_HOME` set that variable takes the place of the `~/.local` entry.
DeaDBeeF has to be **restarted** to dlopen the plugin (`Plugins` in preferences lists it
afterwards).

## Use

1. Start playback.
2. **View → LX Lyrics** spawns the lyric window; the same item ends the session. DeaDBeeF's
   action API has no checkable flag, so the item carries no tick — the wanted state lives in
   `lxlyrics.enabled`, and the plugin keeps that key, the item and the actual child in agreement.
3. Closing the lyric window ends the session and clears the wanted state.
4. The wanted state is **restored at player start**: when `lxlyrics.enabled` is set, the plugin
   spawns the session on the first `DB_EV_PLUGINSLOADED` (after the streamer is up), the same way
   Fooyin's `RememberState` path does. If that restore cannot spawn the app (missing binary, exec
   failure), the key is cleared so the menu item honestly reads off.

Configuration keys in `~/.config/deadbeef/config`:

| Key | Default | Meaning |
|---|---|---|
| `lxlyrics.enabled` | `0` | Wanted desktop-lyrics state; restored at the next player start. |
| `lxlyrics.app_path` | *(empty)* | Path to `lx-lyrics-app`; empty means "search `$PATH`". |

## How the session works

- **Feed** (`src/feed.c`, pure POSIX C — DeaDBeeF has no GLib main loop): two `pipe2()` pipes plus
  `fork()`/`execv()` of `<app_path> --player-feed`, the parent closing the child's ends. The app's
  **stderr is inherited, not piped**, so its log lines land on the player's own stderr (a terminal
  or journal) rather than in a pipe the plugin drains. The first line is
  `{"v":2,"action":"hello","host":"deadbeef","spectrum":<bool>}`; every write goes through one
  mutex as `write()` + `'\n'`.
- **Reader thread**: blocks in `poll()` on the app's stdout with a 500 ms timeout. On timeout it
  emits the periodic `set_status` while playing; on data it parses `get_analyser_data_array` and
  `close_requested`, tolerating/skipping non-JSON lines (a stray Qt stdout write must not break the
  feed). A *well-formed* v2 violation is a protocol error instead (§7): a missing/unsupported `v`,
  an unknown action or a line over 1 MiB is logged loudly and ends the session — the app's stdin is
  closed so it quits on EOF, the toggle goes off, and nothing is respawned. Every write end is
  non-blocking with a bounded wait, so a stalled app can never wedge DeaDBeeF's main thread, and
  `SIGPIPE` is blocked around writes instead of being ignored process-wide. The feed object is
  reference-counted (`lx_feed_acquire`/`lx_feed_release`), because the player thread (plugin
  `message()`), the GTK main thread (the menu action) and the reader thread all touch one session.
- **Message mapping** (`src/plugin.c`): `DB_EV_SONGSTARTED`/`DB_EV_SONGCHANGED` → `set_info` +
  `set_play` while playing; `DB_EV_PAUSED` (p1=1/0) → `set_pause`/`set_play`; `DB_EV_STOP` →
  `set_stop`; `DB_EV_SEEKED` → `set_play(ms)`; `DB_EV_TRACKINFOCHANGED` → a fresh `set_info`, but
  only for the track that is actually playing. `set_info.path` is the `:URI` metadata with a
  leading `file://` stripped; metadata is read under `pl_lock()`/`pl_unlock()`. Toggling on pushes
  the initial snapshot right after the spawn, so the app renders what is already playing. The feed
  thread's notifications (`close_requested`, protocol error, child exit) are marshalled to the
  player thread through plugin-private message ids.
- **Spectrum** (`src/spectrum.c`, protocol §5): `vis_spectrum_listen2` frames are planar floats with
  a stride of `nframes` (4096 bins); the audio thread only copies the first channel into a
  mutex-guarded, preallocated buffer, and the feed thread band-averages it into 128 log-spaced bands
  from 40 Hz to Nyquist and applies `byte = clamp(round(255 * log10(1 + magnitude * 255) /
  log10(256)), 0, 255)`. `hello.spectrum` is `true` only when the listener registered.

## Tests

`tests/feed_test.c` is hermetic: it compiles `feed.c` + `json.c`, spawns a POSIX shell stub as the
"app" (which logs every received line to `$LX_STUB_LOG` and prints the app→host lines itself) and
asserts the exact host→app bytes, the close-requested read-back, the periodic `set_status`, the
non-JSON tolerance, the 128-byte spectrum rule, the no-session/exec-failure paths, each §7
protocol-error case (unsupported `v`, unknown action, over-1 MiB line → reported once and the
session ends, with the app quitting on the EOF it was sent), that a failed write closes the app's
stdin without leaking a descriptor, and that a writer thread keeps working (as a no-op) while
another thread stops the session. No DeaDBeeF headers, player or display server needed:

```sh
cmake -B build -G Ninja -DDEADBEEF_INCLUDE_DIR=/path/to/deadbeef/include && cmake --build build
ctest --test-dir build --output-on-failure
```

The concurrency case is worth running under sanitizers:
`-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"` plus the matching
`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS`; it reports a heap-use-after-free if a session
object is freed while a writer still holds a reference to it.

A live end-to-end run needs DeaDBeeF installed (`command -v deadbeef`): start playback, toggle
**View → LX Lyrics**, and confirm the window follows the track, the seek/pause/stop mapping, the
visualizer (analyser available) and that closing the window ends the session.

## License

GPL-3.0-only (see the SPDX headers); it is a DeaDBeeF plugin and links against DeaDBeeF's API.
