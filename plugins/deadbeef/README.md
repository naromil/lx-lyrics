# lx-lyrics DeaDBeeF adapter

A [DeaDBeeF](https://deadbeef.sourceforge.net/) plugin (`ddb_lxlyrics.so`) that owns the
desktop-lyrics session: it spawns the standalone **lx-lyrics** display as its direct child and
feeds it the player state over the **protocol v2 stdin/stdout player feed**
(`../../docs/protocol.md`). The plugin never reads lyrics — the app owns acquisition (sidecar `.lrc`
+ embedded tags), parsing and rendering.

Closing the lyric window sends `close_requested`; the adapter ends the session, the app exits on
stdin EOF, and the toggle clears itself. The plugin (and its global-shortcut helper) stays loaded,
so the shortcut or **View → LX Lyrics** reopens the window without restarting DeaDBeeF.

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
- `lx-lyrics-hotkey`, the global-shortcut helper (`../../lyrics-app/src/hotkey/`, built when GIO is
  present and installed next to `lx-lyrics-app` by `../../tools/install.sh`). It is looked up next
  to the resolved app, then on `$PATH`; without it the adapter logs one warning and everything
  except the shortcut works.

## Build

CMake (headers via `-DDEADBEEF_INCLUDE_DIR`, or found by CMake's `find_path` when an SDK is
installed):

```sh
cmake -B build -G Ninja -DDEADBEEF_INCLUDE_DIR=/path/to/deadbeef/include
cmake --build build          # -> build/ddb_lxlyrics.so
ctest --test-dir build       # hermetic protocol + shortcut tests, no DeaDBeeF needed
```

A bare `cmake` configures **Debug** (the same default the app project uses); pass
`-DCMAKE_BUILD_TYPE=Release` for the plugin you install.

Or directly, with the same five sources:

```sh
cc -std=c11 -fPIC -shared -O2 -I/path/to/deadbeef/include \
   -o ddb_lxlyrics.so src/plugin.c src/feed.c src/hotkey.c src/json.c src/spectrum.c \
   -lpthread -lm
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

`../../tools/install.sh --player deadbeef` does that (and builds the app) for you, and writes
`lxlyrics.app_path` so the plugin finds `lx-lyrics-app` without a manual path entry.

DeaDBeeF searches, in order: `~/.local/lib64/deadbeef`, `~/.local/lib/deadbeef`, then its system
plugin directory; with `XDG_LOCAL_HOME` set that variable takes the place of the `~/.local` entry.
DeaDBeeF has to be **restarted** to dlopen the plugin (`Preferences → Plugins` lists it
afterwards).

## Use

1. Start playback.
2. **View → LX Lyrics** spawns the lyric window; the same item ends the session, and the global
   shortcut (see *Global shortcut*) is the second way to flip it. DeaDBeeF's action API has no
   checkable flag, so the item carries no tick — the wanted state lives in `lxlyrics.enabled`, and
   the plugin keeps that key, the item, the shortcut and the actual child in agreement.
   The Configuration panel's checkbox is the host's own widget rather than a mirror of the key:
   gtkui does not re-read it after a config write, so a start that fails leaves the key at 0 with
   the tick still drawn, and the panel stays able to write that tick back (see *Configuration*).
   Reopen the panel to see the stored state.
3. Closing the lyric window ends the session and clears the wanted state, but the plugin and its
   helper stay loaded: the next **View → LX Lyrics** or shortcut press starts a fresh session.
   A session that ends by itself never respawns on a playback event.
4. The wanted state is **restored at player start**: when `lxlyrics.enabled` is set, the plugin
   spawns the session on the first `DB_EV_PLUGINSLOADED` (after the streamer is up), the same way
   Fooyin's `RememberState` path does. If that restore cannot spawn the app (missing binary, exec
   failure), the key is cleared so the menu item honestly reads off.
5. **Preferences → Plugins → LX Lyrics** is the plugin's own configuration panel: the app binary
   and the same toggle, in the host's UI. See *Configuration*.

## Configuration

Both keys live in `~/.config/deadbeef/config` and are editable from the host's own plugin page:
**Preferences → Plugins → LX Lyrics → Configuration**. The panel is the `lxlyrics_config_dialog`
layout string in `src/plugin.c`, which gtkui renders and wires to
`deadbeef->conf_get_str`/`conf_set_str` — the module links no GTK and the string *is* the dialog.
A plugin whose `configdialog` is `NULL` gets no panel at all (gtkui hides its button box and the
list's "Only show plugins with configuration" filter drops the plugin), so this layout is what
makes the plugin configurable in the GUI. The host re-applies the whole dialog on every keystroke,
**Reset to defaults** writes the defaults below, and closing the window runs `conf_save()`.

| Key | Default | Meaning |
|---|---|---|
| `lxlyrics.enabled` | `0` | **The lyrics window should be showing** — the **Show desktop lyrics** checkbox, and the state the menu item and the global shortcut flip. Restored at the next player start. |
| `lxlyrics.app_path` | *(empty)* | Path to `lx-lyrics-app` (the **lx-lyrics-app binary** entry); empty means "search `$PATH`". |

The host broadcasts `DB_EV_CONFIGCHANGED` after every config write, and the plugin follows it:
`lxlyrics.enabled` on with no session starts one (a start that cannot spawn clears the key again),
`lxlyrics.enabled` off with a running session ends it. A failed start therefore does not ratchet
the key on, while a panel that is still open can retry: the next keystroke in it rewrites both keys
from the widget state gtkui still holds (a tick the user clicked even though the key was cleared),
which reaches the plugin as another "enabled" write — each retry that cannot spawn clears the key
again, so the key converges to 0 rather than resting on a child that does not exist.
`lxlyrics.app_path` is deliberately **not** acted on there — the dialog re-applies itself on every
keystroke, so restarting a session per character would respawn the app — hence an app-path edit
takes effect at the **next** session start, never mid-session.

## Global shortcut

DeaDBeeF has no hotkey API a plugin can use for this, so the key never passes through the host, and
the adapter carries no portal/D-Bus code. It spawns `lx-lyrics-hotkey` (built from
`../../lyrics-app/src/hotkey/`, installed next to `lx-lyrics-app`) with the write end of a pipe on
**fd 3** and reads a private one-line protocol: `ready <trigger>` once the binding exists,
`unavailable <reason>` when it does not, and `toggle` for every press. A `toggle` only queues a
plugin-private message; the flip happens on DeaDBeeF's **player thread**, exactly like the View/LX
Lyrics action, so the helper's reader thread never touches the session or the GTK side.

The helper holds the key through the XDG desktop portal
(`org.freedesktop.portal.GlobalShortcuts`). The adapter asks for **`CTRL+ALT+L`** as the
`preferred_trigger`; the desktop assigns the real combination and reports it back, and the adapter
logs `lx-lyrics: global shortcut bound to <trigger>`.

**The app id comes from the systemd user unit.** The portal only accepts a shortcut for a caller
whose unit is named `app-<app-id>@<uuid>.service`. A player started from the KDE launcher has one;
a player started from a terminal has none, so the portal answers
`org.freedesktop.portal.Error.NotAllowed: An app id is required`. The helper reports
`unavailable ...` on the pipe and exits 0 — a normal outcome, not a failure: the adapter logs it
once, the shortcut is simply absent, and the menu item and settings page keep working.

**Re-binding.** `CTRL+ALT+L` is only the *preferred* trigger offered when the helper starts. The
binding belongs to the desktop: open **System Settings → Shortcuts** (KDE), find DeaDBeeF under its
own application group, and change or clear the key there. The helper exits with the player; the
binding is re-established on the next start.

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
  feed). An analyser request is answered only when this session's `hello.spectrum` was `true`: a
  host that declared no analyser MUST never receive that request (§4), so when the declaration was
  `false` it is a protocol error, not an action to ignore — otherwise a broken app would wait for a
  frame forever. Any other *well-formed* v2 violation is a protocol error too (§7): a
  missing/unsupported `v`, an unknown action or a line over 1 MiB is logged loudly and ends the
  session — the app's stdin is closed so it quits on EOF, the toggle goes off, and nothing is
  respawned. Every write end is
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
- **Global shortcut** (`src/hotkey.c`, pure POSIX C, no DeaDBeeF header): started with the plugin
  and stopped in its `stop()` callback before the session is torn down, so the key works while the
  window is hidden and cannot outlive the player. Its reader thread only logs the status line and
  calls `deadbeef->sendmessage(LX_EV_HOTKEY_TOGGLE)`; the player thread applies the toggle through
  the same switch as the menu item (a queued toggle is ignored once shutdown has started).
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
protocol-error case (unsupported `v`, unknown action, over-1 MiB line, and `get_analyser_data_array`
against a `spectrum: false` hello → reported once, the analyser callback never runs, the session
ends with the app quitting on the EOF it was sent; the same request against `spectrum: true` reaches
the callback once and is answered), that a failed write closes the app's
stdin without leaking a descriptor, and that a writer thread keeps working (as a no-op) while
another thread stops the session.

`tests/hotkey_test.c` is hermetic too: it compiles `hotkey.c` on its own and drives it with `/bin/sh`
stand-ins that speak the helper protocol on fd 3 (the `--pipe-fd` argument, `ready`/`unavailable`
status lines, repeated `toggle` presses, a helper that never answers, a missing/throwing helper,
stop while the reader is busy). Both suites need no DeaDBeeF headers, player or display server:

```sh
cmake -B build -G Ninja -DDEADBEEF_INCLUDE_DIR=/path/to/deadbeef/include && cmake --build build
ctest --test-dir build --output-on-failure   # feed_test + hotkey_test
```

The concurrency case is worth running under sanitizers:
`-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"` plus the matching
`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS`; it reports a heap-use-after-free if a session
object is freed while a writer still holds a reference to it.

A live end-to-end run needs DeaDBeeF installed (`command -v deadbeef`): start playback, toggle
**View → LX Lyrics**, and confirm the window follows the track, the seek/pause/stop mapping, the
visualizer (analyser available) and that closing the window ends the session. With `lx-lyrics-hotkey`
installed and the desktop granting the binding, the same run should show a `global shortcut bound
to ...` log line and the key should open and close the window.

## License

GPL-3.0-only (see the SPDX headers); it is a DeaDBeeF plugin and links against DeaDBeeF's API.
