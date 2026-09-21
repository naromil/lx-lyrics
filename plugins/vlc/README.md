# lx-lyrics VLC adapter

A [VLC](https://www.videolan.org/) interface module (`liblxlyrics_plugin.so`) that owns the
desktop-lyrics session: it spawns the standalone **lx-lyrics** display as its direct child and
feeds it the player state over the **protocol v2 stdin/stdout player feed** (`../../docs/protocol.md`).
The module never reads lyrics — the app owns acquisition (sidecar `.lrc` + embedded tags), parsing
and rendering.

It is loaded as a *background interface* with `--extraintf=lxlyrics` (Preferences → Interface →
"Extra interface modules"), so it runs with or without a GUI and starts with the player. Closing
the lyric window sends `close_requested`; the adapter ends the session, the app exits on stdin EOF,
and the module's own "Desktop lyrics" setting turns itself off (a restart brings it back — see
[Use](#use)).

## Requirements

- **VLC 3.0.x.** Built and verified against **3.0.23** (headers *and* a live run). Every call used
  here is 3.0-era public API, but only 3.0.23 was actually compiled and run against.
  **VLC 4 is not supported** — not "untested", unsupported: VLC 4 looks the plugin up by a bare
  `vlc_entry` symbol plus a separate `vlc_entry_api_version` (master `src/modules/bank.c:212`,
  `:268`, `:640`), while 3.0 looks up `vlc_entry__3_0_0f` (`src/modules/bank.c:204`,
  `include/vlc_plugin.h:192`), and the input API this module is built on is gone from the public
  headers: `input_thread_t` no longer exists in master's `include/vlc_input.h` and
  `playlist_CurrentInput()`/`playlist_Lock()` are gone from `include/vlc_playlist.h` (replaced by
  the `vlc_player_t` API in `include/vlc_player.h`). A 3.0 build cannot even load into VLC 4.
- **VLC's module headers**, from the distribution (they ship a `vlc-plugin` pkg-config module) or
  from a source checkout:
  ```sh
  git clone --depth 1 --branch 3.0.23 https://github.com/videolan/vlc /tmp/vlc-src
  ```
- A **C11** compiler. VLC 3.0 requires C11 (`configure.ac:48` `VLC_PROG_CC_C11`, `m4/c11.m4:12-29`)
  and its headers are built on it (`vlc_common.h:480` uses `_Generic`), so `plugin.c` is compiled
  as C11; the hermetic sources (`feed.c`, `json.c`, `mapper.c`) stay strict C99.
- `lx-lyrics-app` installed (e.g. by `../../tools/install.sh`), findable on `$PATH` (or set
  `lxlyrics-app-path`).

### Upstream facts this module depends on

Every claim below was read out of a 3.0.23 checkout (`file:line`); the ones marked *(verified)* were
also checked against the running system.

| Fact | Proof |
|---|---|
| An interface module is a `set_capability("interface", …)` module; `intf_Create()` creates an `intf_thread_t` and selects it with `module_need(p_intf, "interface", module, true)`. | `src/interface/interface.c:71`, `:109` |
| `extraintf` is a colon-separated *module list* in the Interface category, labelled "Extra interface modules". | `src/libvlc-module.c:2169-2170`, `:74-79` |
| Background interfaces are started by libvlc at init: it reads `extraintf` (+ `control`), splits on `:`, and loads each as `<name>,none` — "this module or none", no fallback. | `src/libvlc.c:301-336` |
| They are stopped at shutdown by `intf_DestroyAll()` → `module_unneed()` → the module's `Close`. | `src/interface/interface.c:238-252` |
| VLC only loads files matching `lib*_plugin.so`; the *file name* is the module name: the build strips `lib`/`_plugin*` and passes `-DMODULE_STRING=…`. That string is the module's shortcut, i.e. what `--extraintf=` matches. | `src/modules/bank.c:418-427`; `modules/common.am:18-27`; `src/modules/entry.c:288-296`; `src/modules/modules.c:165` |
| The entry symbol is `vlc_entry` + the ABI suffix: `vlc_entry__3_0_0f`. | `src/modules/bank.c:204`; `include/vlc_plugin.h:192` |
| A plugin is a `-module` shared object exporting only its entry points, linked against `libvlccore`. *(verified)* | `modules/common.am:37-43`; `readelf -d /usr/lib/vlc/plugins/misc/libaudioscrobbler_plugin.so` → `NEEDED libvlccore.so.9`; `nm -D --defined-only` → `vlc_entry__3_0_0f`, `vlc_entry_copyright__3_0_0f`, `vlc_entry_license__3_0_0f` |
| `pkg-config vlc-plugin` supplies exactly those flags: `-I<pkgincludedir>/plugins -D__PLUGIN__ … -lvlccore`; the module headers live in `<includedir>/vlc/plugins`. | `vlc-plugin.pc` *(verified)*; `src/Makefile.am:22-24` |
| Out-of-tree modules must define `_()`/`N_()` themselves: in-tree builds get them from `vlc_fixups.h`, which `configure` injects through `config.h` and which is **not installed**. | `configure.ac:713` (`AH_BOTTOM([#include <vlc_fixups.h>])`); `src/Makefile.am:114` (`noinst_HEADERS`) |
| `vlc_objects.h` is the one header **without an include guard** — including it again after `vlc_common.h` is a hard error. | `include/vlc_objects.h`; `vlc_common.h:1037` |
| The current input is obtained with `pl_CurrentInput()` / `playlist_CurrentInput()`, which take the playlist lock and return a **held reference** to release with `vlc_object_release()`. | `include/vlc_interface.h:93-102`; `include/vlc_playlist.h:315`; `src/playlist/engine.c:371-391`; `include/vlc_objects.h:51` |
| Playback state, position and rate are the input's `state` (`input_state_e`), `time` and `rate` variables; `time` is in **microseconds** (`MS_FROM_VLC_TICK()` converts). | `include/vlc_input.h:283-293`, `:296-315`; `src/input/var.c:134-147`; `include/vlc_mtime.h:49-56`; `src/input/input.c:1867` divides the same variable by `CLOCK_FREQ` to get seconds |
| Metadata comes from the input item: `input_GetItem()`, `input_item_GetURI()`, `input_item_GetTitleFbName()`, `input_item_GetArtist()`, `input_item_GetAlbum()` (heap strings, item lock held internally). | `include/vlc_input.h:543`; `include/vlc_input_item.h:262-266`, `:273-282` |
| A `file://` URI becomes a local path (with `%XX` decoded) through `vlc_uri2path()`; anything else returns NULL — streams get `path: ""`. | `include/vlc_url.h:58`; `src/text/url.c:241-300` |
| Fullscreen *is* queryable: the playlist owns a `fullscreen` bool, and the Qt interface mirrors the video output's state onto it. | `src/playlist/engine.c:471`; `modules/gui/qt/input_manager.cpp:1290-1291`, `actions_manager.cpp:146` |
| **No analyser API is reachable from a plugin.** The only spectrum consumers are `visual` *video-output effect* modules (`modules/visualization/visual/visual.h` is a module-private header), and the installed plugin headers contain no visualisation interface. The handshake therefore always says `spectrum: false`. | `modules/visualization/visual/visual.h`; no `vlc_visualizations.h` among the installed headers *(verified)* |
| Config values written by a module are persisted: `config_PutInt()` marks the config dirty and VLC saves it to `vlcrc` at exit. | `include/vlc_configuration.h:100`; `src/config/core.c:225-243`; `src/config/file.c:525-540`; `src/libvlc.c:422-423` |
| Module options appear in **Tools → Preferences → (Show settings: All)** under their category, because that tree walks every *banked* (discovered) module — not only the loaded ones: the module's own node is there with the module never instantiated. | `modules/gui/qt/components/complete_preferences.cpp:87-96`, `:101-178`, `:198-267` *(verified: the node and its two options render with `extraintf` empty)* |
| Plugins are found in `<libdir>/plugins` and in every directory of `$VLC_PLUGIN_PATH`, and the directory is *scanned* at every start (`plugins-scan` defaults to true) — an unknown file is loaded from disk, so **`vlc-cache-gen` is not required** to install one. | `src/modules/bank.c:530-565`, `:469-505`, `:271-303`; `src/libvlc-module.c:2033-2036` |
| A plugin cannot be an extension instead: extensions are hosted by the Qt interface and are activated from the View menu, and the Lua API they run on gives them at most a unidirectional `io.popen` — one pipe, while the feed needs the child's stdin *and* stdout. | `modules/gui/qt/extensions_manager.cpp:43-66`; `include/vlc_extensions.h:63-81`; `modules/lua/extension.c:830` |

### The trap worth knowing about

`vlc_clone()` blocks `SIGHUP`/`SIGINT`/`SIGQUIT`/`SIGTERM`/`SIGPIPE` around `pthread_create()`
(`src/posix/thread.c:430-441`), and a new thread inherits the creating thread's mask. The adapter's
session thread is created that way, so its `fork()`+`execv()` used to hand the display app a
blocked mask — which survives `exec()` — making the app immune to `SIGTERM`/`SIGINT` (observed:
`/proc/<pid>/status` showed `SigBlk: 0000000000015007` in the app VLC had spawned) and turning the
adapter's own `SIGTERM` escalation into a no-op. `feed.c` resets the child's mask to the empty set
before `execv()`, and `tests/feed_test.c` spawns from a thread carrying exactly VLC's mask to keep
it that way.

## Build

CMake (headers via pkg-config, or explicitly):

```sh
cmake -B build -G Ninja                      # uses pkg-config vlc-plugin
cmake -B build -G Ninja -DVLC_INCLUDE_DIR=/tmp/vlc-src/include   # or a source checkout
cmake --build build                          # -> build/liblxlyrics_plugin.so
ctest --test-dir build                       # hermetic protocol + mapping tests, no VLC needed
```

A bare `cmake` configures **Debug** (the same default the app project uses); pass
`-DCMAKE_BUILD_TYPE=Release` for the plugin you install. The VLC headers are a **hard requirement**
for the module target (the configure step fails with instructions when they are missing), while the
two test targets never see a VLC header.

Or directly, with the same four sources and the flags VLC's own build uses:

```sh
cc -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra -O2 \
   -DMODULE_STRING='"lxlyrics"' -D__PLUGIN__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE \
   -I/usr/include/vlc/plugins \
   -shared -o liblxlyrics_plugin.so src/plugin.c src/feed.c src/json.c src/mapper.c -lvlccore
```

Both forms were verified: the module compiles warning-free against the installed 3.0.23 headers
*and* against the `/tmp/vlc-src/include` headers with the in-tree flags above.

The file name matters twice: VLC only loads `lib*_plugin.so`, and `MODULE_STRING` — which is what
`--extraintf=` matches — must be the name inside it (`lxlyrics`). The build pins both, and the
result exports exactly `vlc_entry__3_0_0f` plus the copyright metadata.

## Install

System-wide (needs root):

```sh
install -m 755 build/liblxlyrics_plugin.so /usr/lib/vlc/plugins/misc/
```

Without root, put it anywhere and point VLC's plugin path at that directory:

```sh
mkdir -p ~/.local/lib/vlc/plugins
install -m 755 build/liblxlyrics_plugin.so ~/.local/lib/vlc/plugins/
VLC_PLUGIN_PATH=~/.local/lib/vlc/plugins vlc --extraintf=lxlyrics
```

`VLC_PLUGIN_PATH` is read from the environment of the VLC process, so this suits terminal launches;
a desktop-launched VLC does not read your shell profile — use the system plugin directory, or set
the variable in the launcher's environment.

**No `vlc-cache-gen` step is needed.** VLC scans the plugin directories at every start
(`plugins-scan` defaults to true) and loads a file its cache does not know straight from disk
(`src/modules/bank.c:271-303`, `:469-505`). Run `vlc-cache-gen <plugin-dir>` (or start VLC with
`--reset-plugins-cache`) only if you disabled the scan.

`../../tools/install.sh --player vlc` builds the module, installs it into the scanned system
directory when that is writable and into `~/.local/lib/vlc/plugins` otherwise, writes
`[lxlyrics] lxlyrics-app-path` and — when VLC will find the module without extra environment —
adds `lxlyrics` to `[core] extraintf`. For a per-user install it prints the
`VLC_PLUGIN_PATH=… vlc --extraintf=lxlyrics` launch line instead.

VLC has to be **restarted** to pick the module up — `extraintf` is read once, while libvlc starts
(`src/libvlc.c:301`).

## Use

1. Start playback, then start VLC with the module enabled:
   ```sh
   vlc --extraintf=lxlyrics
   ```
   In the GUI this is **Tools → Preferences → Show settings: All → Interface → Main interfaces →
   "Extra interface modules"**, where the module is listed as a checkbox labelled with its own
   description, **"Desktop lyrics (lx-lyrics)"**, so ticking it is enough (or add
   `lxlyrics` to the colon-separated list, e.g. `rc:lxlyrics`); then click **Save** and restart VLC.
   "Show settings: **All**" is not optional: VLC's Simple pages are hand-written panels bound to
   literal option names (`config_FindConfig("qt-notification")` and friends), so no out-of-tree
   module can ever appear on them. In `vlcrc` the same two settings are:

   ```ini
   [core]
   extraintf=lxlyrics

   [lxlyrics] # Desktop lyrics (lx-lyrics)
   lxlyrics-enabled=1
   #lxlyrics-app-path=
   ```

   The module's own options are in the same "All" settings tree under **Interface → Main
   interfaces → "LX Lyrics"** (the module's `set_shortname()`): **Desktop lyrics** and
   **Path to lx-lyrics-app**, and on the command line as `--lxlyrics-enabled` /
   `--lxlyrics-app-path` — the boolean takes VLC's bare `--lxlyrics-enabled` /
   `--no-lxlyrics-enabled` form (`--lxlyrics-enabled=0` is rejected by VLC's option parser).
2. The lyric window appears with the session. It follows the playing track, the pause/seek/stop
   state, and VLC's fullscreen state (with `desktopLyric.fullscreenHide` the app hides itself while
   VLC is fullscreen).
3. **Closing the lyric window** (its control-bar close button or the WM close) sends
   `close_requested`: the adapter ends the session, the app quits, and `lxlyrics-enabled` is turned
   **off** — persisted, so a later VLC start does not bring the window back unasked.
4. **To bring it back**, set `lxlyrics-enabled` to true again (Preferences → All → Interface →
   "Desktop lyrics", or `lxlyrics-enabled=1` in `vlcrc`, or `--lxlyrics-enabled` on the command
   line) and **restart VLC**. There is no in-process re-enable: `extraintf` modules are created once
   during libvlc startup (`src/interface/interface.c:198`), so a restart is inherent — which is also
   why this is the honest documented path rather than a menu item that could not work.

| Config key | Default | Meaning |
|---|---|---|
| `lxlyrics-enabled` | `1` | Wanted desktop-lyrics state. Read when the module is created; turned off when a session ends (user close, protocol error, the app quitting, a failed spawn) and kept when VLC itself shuts down, so a restart restores it. |
| `lxlyrics-app-path` | *(empty)* | Path to `lx-lyrics-app`; empty means "search `$PATH`". |

## How the session works

- **Transport** (`src/feed.c`, pure POSIX C — no VLC headers): two `pipe2()` pipes plus
  `fork()`/`execv()` of `<app_path> --player-feed` as a **direct child**, argv array, never a shell.
  The app's **stderr is inherited, not piped**, so its log lines land in VLC's own log/stderr. The
  first line is `{"v":2,"action":"hello","host":"vlc","spectrum":false}`; the prime tick then
  pushes the full snapshot (`set_info` + `set_play`/`set_pause`/`set_stop` + `set_fullscreen`), so a
  session started during playback shows the current track. Every write goes through one mutex as
  `write()` + `'\n'`, on a non-blocking write end with a bounded wait, so a stalled app can never
  wedge VLC; `SIGPIPE` is blocked around writes instead of being ignored process-wide.
- **The 500 ms poll is the whole clock.** VLC 3.0 has no playback event a plugin can subscribe to
  from its own thread without holding an input object across callbacks (`intf-event` callbacks run
  on the input thread and every in-tree consumer — `modules/misc/audioscrobbler.c:347-436`,
  `modules/control/dbus/dbus.c:1160-1194` — has to add/remove them per input and keep a reference
  alive). Instead the feed's reader thread calls the sampler every 500 ms, and each tick does one
  self-contained sample: `pl_CurrentInput()` → `state`/`time`/`rate` → the input item's URI,
  title/artist/album → `vlc_object_release()`. No VLC pointer survives a tick, and no VLC lock is
  held while the feed writes. The state/seek/track diffs are computed from the polled samples by
  `src/mapper.c`, which is VLC-free and unit-tested.
- **Mapping** (`src/plugin.c` samples, `src/mapper.c` decides, `src/feed.c` writes):

  | VLC (`input_state_e`) | sample | emitted |
  |---|---|---|
  | `INIT_S`, `END_S`, `ERROR_S`, no input at all | stopped | `set_stop` |
  | `OPENING_S`, `PLAYING_S` | playing | `set_play(played_time)` |
  | `PAUSE_S` | paused | `set_pause` |

  | Change between two samples | emitted |
  |---|---|
  | first sample after the spawn (the prime) | `set_info` + state action + `set_fullscreen` |
  | track change (path changed, or an input appeared) | `set_info` + state action |
  | metadata edit on the same path (artist/title/album) | `set_info` only — the timer keeps running |
  | state change | `set_play(played_time)` / `set_pause` / `set_stop` |
  | seek: still playing, same track, position more than 2 s off the predicted one | `set_play(played_time)` |
  | fullscreen changed | `set_fullscreen(isFullscreen)` |

  `played_time` is the input's `time` converted from microseconds with `MS_FROM_VLC_TICK()`, and
  `path` is the item's URI run through `vlc_uri2path()` (`""` for streams, which the app renders as
  "no lyrics" rather than an error). A state change is pushed as its state action
  (`set_play`/`set_pause`/`set_stop`) at the next tick; `set_status` stays what protocol §5 defines
  it to be — the periodic position clock while playing — so no message is duplicated on a state
  change (the same choice the DeaDBeeF and Rhythmbox adapters make). The seek prediction advances by
  `rate` per elapsed millisecond, so a non-1.0 playback rate is not mistaken for a seek; the rate
  itself is never sent (`set_playbackRate` is reserved for rate-capable hosts and this adapter
  deliberately does not use it — the app recomputes the active line from the position anyway). A
  seek smaller than the tolerance needs no message either: the periodic `set_status` carries the
  absolute position.
- **App→host parsing is strict** (protocol §4/§7): exactly `close_requested`, which ends the session
  and never respawns. `get_analyser_data_array` is a **protocol error** here, not an action to
  ignore: §4 forbids the request from a host that declared `spectrum: false` (the same call the
  Rhythmbox adapter makes). A missing/unsupported `v`, an unknown action, a malformed payload or a
  line over 1 MiB is logged loudly and ends the session — the app's stdin is closed so it quits on
  EOF, the key is turned off, and nothing is respawned. A line that is not a JSON object is skipped
  *loudly* (one bounded log line) and the session continues.
- **Threading and lifetimes.** VLC's main thread runs `Open()`/`Close()` (`intf_Create()` at
  `src/interface/interface.c:71`, `intf_DestroyAll()` at `:238`). `Open()` starts the adapter's
  **session thread**, which spawns the app and — because `lx_feed_stop()` joins the feed's reader
  thread — is also the only thread that stops it (`src/feed.h`); keeping that off VLC's main thread
  is what stops a slow app from blocking VLC. `Close()` flags the end and joins it. The feed's
  **reader thread** runs the sampler and the end-of-session callbacks and touches VLC only through
  the thread-safe getters above; the mapper belongs to it alone. `Close()` keeps the wanted state
  (`keep_enabled`), a session that ends by itself turns the key off.
- **No lyric logic anywhere.** The module sends `path` + display metadata and leaves the lyric keys
  empty; acquisition, parsing, selection and rendering are the app's (`docs/protocol.md` §1).

## Tests

`tests/feed_test.c` is hermetic: it compiles `feed.c` + `json.c`, spawns a POSIX shell stub as the
"app" (which logs every received line to `$LX_STUB_LOG` and prints the app→host lines itself) and
asserts the exact host→app bytes — the `vlc` handshake, the snapshot the prime pushes right after it,
the periodic `set_status` cadence, the `close_requested` read-back, the non-JSON tolerance, each §7
protocol-error case (unsupported `v`, unknown action, `get_analyser_data_array` against a
`spectrum:false` handshake, over-1 MiB line → reported once, session ended, with the app quitting on
the EOF it was sent), the no-session/exec-failure paths, that a failed write closes the app's stdin
without leaking a descriptor, that a wedged app which ignores `SIGTERM` is still killed (bounded), and
that the app is spawned with an **empty signal mask** even when the spawning thread carries VLC's.
`tests/mapper_test.c` covers the mapping decisions: prime, steady playback, metadata edit, track
change, pause/resume/stop, the seek heuristic (tolerance edges, backwards clock, doubled/halved and
invalid rates), fullscreen changes and stream inputs.

No VLC, player or display server is needed:

```sh
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

The concurrency cases (a writer hammering a session that is stopped from another thread; the
spawning thread carrying VLC's signal mask) are worth running under sanitizers:
`-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"` plus the matching
`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS`; they report a heap-use-after-free if a session
object is freed while a writer still holds a reference to it. Both suites pass clean that way.

## Live end-to-end

A real VLC run needs nothing but the plugin and a playable file:

```sh
VLC_PLUGIN_PATH=/tmp/lx-vlc-plugins QT_QPA_PLATFORM=offscreen vlc \
    -I dummy --extraintf=lxlyrics --aout=adummy --vout=vdummy --no-video \
    /tmp/lx-smoke-track.flac
```

Observed with 3.0.23 (the app is at `~/.local/bin/lx-lyrics-app`; its stderr is VLC's stderr, so its
own log lines appear inline):

```
lxlyrics interface: starting the desktop-lyrics session (/home/naromil/.local/bin/lx-lyrics-app)
feed: connected to "vlc" spectrum: false
feed: track "<unknown>" "" "" playing: false lyric chars: 0
feed: track "lx-smoke-track.flac" "" "[/tmp/lx-smoke-track.flac]" playing: true lyric chars: 0
```

— the module was selected by name, spawned the app as its child, and pushed the track with its
decoded local path and `isPlay: true` as soon as playback started. Killing the app (or quitting VLC)
then produced:

```
lxlyrics interface: ending the session (the app exited)
lxlyrics interface: desktop lyrics are off; set "lxlyrics-enabled" back to true and restart VLC …
```

and `vlcrc` gained `[lxlyrics] lxlyrics-enabled=0` — the honest off state, as documented above.
Starting VLC again with the key at `0` logged `desktop lyrics are off ("lxlyrics-enabled" is false)`
and spawned nothing, while a normal VLC shutdown left the key at `1` for the next start.

Two environment notes from that run: VLC 3.0.23 with `-I dummy` does not exit by itself after the
playlist ends on this box (the same command *without* the plugin behaves identically — verified), so
wrap it in `timeout` or quit VLC normally; and the app needs a display-less platform only for the
smoke test (`QT_QPA_PLATFORM=offscreen`), never in normal use.

The same wire sequence can be driven against the app alone, without VLC, with any build of the feed
layer (`printf '…' | QT_QPA_PLATFORM=offscreen lx-lyrics-app --player-feed`): the app answers the
`vlc` handshake, acquires the lyrics for the `set_info.path` it is given and quits with status 0 on
stdin EOF, as `docs/protocol.md` §2 requires.

## License

GPL-3.0-only (see the SPDX headers). VLC is GPL-2.0-or-later, whose "or later" clause allows a
GPL-3.0-only module — the same argument the Rhythmbox adapter documents. VLC's own module metadata
macros have no GPL-3.0 entry (only `VLC_LICENSE_LGPL_2_1_PLUS` and `VLC_LICENSE_GPL_2_PLUS`,
`include/vlc_plugin.h:522-531`), so the module exports its copyright but no license string; the
SPDX header is the statement.
