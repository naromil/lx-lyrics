# LX Lyrics — Quod Libet plugin

A Quod Libet (event-plugin, Python) adapter for the standalone **lx-lyrics** desktop
lyrics display. The plugin observes Quod Libet's player through the event-plugin API,
spawns `lx-lyrics-app --player-feed` as its **direct child** and streams
newline-delimited v2 JSON to the child's stdin. The app owns everything else: it reads the
sidecar/embedded lyrics of the file at `path`, parses them, and renders them.

The lyric window therefore lives and dies with Quod Libet: closing the window ends the
session, and the app quits when its stdin closes.

## Install

Quod Libet scans `$XDG_CONFIG_HOME/quodlibet/plugins` (`~/.config/quodlibet/plugins` on
Linux) for per-user plugins, so no root is needed:

```sh
mkdir -p ~/.config/quodlibet/plugins
cp plugins/quodlibet/lxlyrics.py ~/.config/quodlibet/plugins/lxlyrics.py
```

Unlike the Rhythmbox adapter there is **no `.plugin` metadata file**: Quod Libet loads
plain `.py` modules and packages (a directory with `__init__.py`) and reads the plugin's
metadata from class attributes. Copy the single module as shown — Quod Libet's scanner
walks subdirectories, so dropping the whole `plugins/quodlibet/` directory into the plugins
folder would also try to import `tests/test_lxlyrics.py` as a plugin.

`../../tools/install.sh --player quodlibet` does that copy and writes `lxlyrics_app_path`
into Quod Libet's config (through Quod Libet's own config library) for you.

Then:

1. Restart Quod Libet and open **Music → Plugins**. (The Plugins window's *Refresh*
   button, which upstream's plugin docs mention, is only shown in debug builds —
   `quodlibet/qltk/pluginwin.py:424-429`.)
2. Find **LX Lyrics** (the search box accepts the name or the id `lxlyrics`) and switch it
   on. That toggle *is* the session toggle — the plugin starts the display app the moment
   it is enabled, so a freshly installed plugin never opens a window on its own.
3. If `lx-lyrics-app` is not on `PATH`, set its path in the same Plugins window: select
   **LX Lyrics** and use the *Lyrics app executable* entry in its preferences pane. The
   app is always started as `<executable> --player-feed`.

Closing the lyric window (its control bar or the WM close button) turns the plugin back
off and remembers that, so it does not come back on the next start; switch it on again in
**Music → Plugins** to get the window back. Turning the plugin off there ends the session
the same way, and so does the app exiting on its own.

An event plugin has no menu of its own to hang a second toggle on — Quod Libet's other UI
hook for plugins is a sidebar (`UserInterfacePlugin`, `quodlibet/plugins/gui.py:15-31`),
which a lyrics window has no use for — so the plugin manager's own toggle is the only
honest one.

Ex Falso shares the plugin directory but registers only a songs-menu handler
(`quodlibet/qltk/exfalsowindow.py:55-59`), so this plugin is never loaded there.

## Requirements

- **Quod Libet ≥ 4.4.0.** The adapter uses the user plugin directory, the
  `EventPlugin` event hooks, `PluginConfigMixin` (its option storage) and
  `PluginManager.enable()`/`save()` (the runtime on/off the close path needs). All four are
  present in 4.4.0 and unchanged in 4.7.1, which is the release every `file:line`
  citation below was read from; see *Upstream API* for the exact call sites.
- **GTK 3 / PyGObject.** Quod Libet 4.7.1 is a GTK 3 application and pins it itself
  (`quodlibet/qltk/__init__.py:17`); the adapter pins `Gdk`/`Gtk` 3.0 with it.
- The lx-lyrics display app (this repository's `lyrics-app`), built and installed.

Quod Libet exposes **no analyser to plugins**, so the handshake declares
`spectrum: false` and the app never requests spectrum frames. Quod Libet *does* have a
queryable fullscreen state, so `set_fullscreen` is implemented (see *Wire contract*).

## Configuration

The plugin has exactly one setting, and it lives in Quod Libet's own config file —
`$XDG_CONFIG_HOME/quodlibet/config` (`quodlibet/main.py:24`), section `[plugins]`, option
`lxlyrics_app_path` (the name `PluginConfigMixin` derives from `PLUGIN_ID`, see
*Upstream API*):

```ini
[plugins]
lxlyrics_app_path=/usr/local/bin/lx-lyrics-app
```

- **Default**: the bare name `lx-lyrics-app`, which GLib resolves against `$PATH` when the
  child is spawned. Nothing is written back until you change it.
- **Override**: the *Lyrics app executable* entry in the plugin's preferences pane, or the
  key above. Quod Libet writes the config on exit and every five minutes
  (`quodlibet/main.py:227`, `quodlibet/_main.py:413-417`).

## Debugging

The app's stderr is inherited, so its own logs land in Quod Libet's console (run
`quodlibet` from a terminal). The adapter logs through Quod Libet's own helpers: protocol
errors, spawn failures and write failures go to the error channel, which is always
printed; the stalled-child warning and the informational session transitions go to the
warning/debug channels, which Quod Libet prints only in debug mode
(`quodlibet/util/dprint.py:172-204,283-295`). Quod Libet's debug flag is `--debug` (or the
`QUODLIBET_DEBUG` environment variable, `quodlibet/const.py:291`):

```sh
quodlibet --debug
QUODLIBET_DEBUG=1 quodlibet   # the same
```

## Tests

Hermetic — no Quod Libet, GTK, PyGObject, display server, player or app binary needed. The
adapter is imported against stub `gi.repository`, `quodlibet`, `quodlibet.plugins`,
`quodlibet.plugins.events`, `quodlibet.qltk` and `quodlibet.util.dprint` modules, and the
tests assert the exact JSON lines emitted for scripted player events (handshake, snapshot,
track start/end, pause/unpause, seek, the 500 ms `set_status` cadence, the seek
heuristic, fullscreen transitions, `close_requested`), plus the process lifecycle
(grace-then-kill, protocol errors, non-JSON tolerance, the capped pending queue, short and
failed writes) and the config round-trip.

```sh
python3 -m unittest discover -s plugins/quodlibet/tests -v   # 56 tests
```

(From inside this directory, `python3 -m unittest discover -s tests -v` is equivalent.)

## Wire contract

One JSON object per UTF-8 line. The first line written is the handshake:

```json
{"v":2,"action":"hello","host":"quodlibet","spectrum":false}
```

Host → app: `set_info` (path, singer, name, album, empty lyric fields — the app reads the
file at `path`), `set_status`, `set_play`, `set_pause`, `set_stop`, and `set_fullscreen`
on every change of the main window's fullscreen state. A session starts with
`hello`, a full `set_info`, one `set_play`/`set_pause`/`set_stop`, then the initial
`set_fullscreen`.

App → host: `close_requested` only — any other well-formed line (a `v` that is not 2, an
unknown action, an analyser request this host never declared, a line over 1 MiB) is a
protocol error that ends the session, while non-JSON stdout noise is skipped. See
`docs/protocol.md` for the full v2 contract.

### How the session works

| Quod Libet | adapter |
|------------|---------|
| plugin enabled (or restored at startup) | spawn the child, `hello`, then the `set_info` + state snapshot of whatever is playing |
| `plugin_on_song_started(song)` | `set_info` + `set_play{0}` — a fresh track starts at 0 |
| `plugin_on_song_ended(song, stopped)` | `set_stop` when `stopped` (playback really ended, not a gapless move) |
| `plugin_on_paused()` / `plugin_on_unpaused()` | `set_pause` / `set_play{player position}` |
| `plugin_on_seek(song, msec)` | `set_play{msec}` |
| 500 ms poll while playing | `set_status{isPlay, played_time}`, or `set_play` when the clock jumped a second or more (a seek the player did not report) |
| main window fullscreen changed | `set_fullscreen{isFullscreen}` |
| plugin disabled, or Quod Libet quitting | close the child's stdin (EOF ⇒ the app quits); `force_exit()` only after `STOP_GRACE_MS` |
| app `close_requested` | end the session without killing the app, and turn the plugin off through the plugin manager |
| app exits on its own, protocol error, spawn failure | turn the plugin off, no respawn |

Everything runs on Quod Libet's GTK main loop: the child's stdout is read with one armed
async line read at a time, and its stdin is non-blocking (`os.set_blocking`), so a child
that stops draining leaves the pending write waiting for `POLLOUT` instead of parking the
main loop. Queued lines are capped at `MAX_PENDING_LINES` (64) with the oldest stale lines
dropped — never the line being written and never the leading `hello` — and one warning per
overflow episode.

Two edge cases are inherited from the Rhythmbox adapter on purpose, because they are the
same protocol machinery:

- If a write is still in flight when the session ends, GLib refuses to close the stream
  (`G_IO_ERROR_PENDING`); the adapter reports nothing, waits out `STOP_GRACE_MS` and then
  kills the child. In the real app that means a kill rather than an EOF exit when the
  plugin is switched off in the same instant a `set_status` line is being written.
- `close_requested` never kills the child: the app is closing its own window and gets to
  run its close animation.

## Upstream API

Read from the Quod Libet source at tag `release-4.7.1`
(`git clone --depth 1 --branch release-4.7.1 https://github.com/quodlibet/quodlibet`).

**Plugin discovery and metadata.** Quod Libet scans `<get_user_dir()>/plugins`
(`quodlibet/_main.py:237`) plus its own `ext/<kind>` folders (`quodlibet/_main.py:22-31`),
where `get_user_dir()` is `$XDG_CONFIG_HOME/quodlibet` on Linux
(`quodlibet/_main.py:160-168`). The scanner takes plain `.py` files and directories
containing an `__init__.py` — no `.plugin` file exists in Quod Libet
(`quodlibet/util/importhelper.py:49-84`). A module object is a plugin when it has
`PLUGIN_ID`; `PLUGIN_NAME` defaults to the id, and `PLUGIN_DESC`, `PLUGIN_ICON`,
`PLUGIN_TAGS`, `PLUGIN_CAN_ENABLE` and `PLUGIN_INSTANCE` are read where present
(`quodlibet/plugins/__init__.py:108-130`, `:139-197`). `PLUGIN_VERSION` and
`PLUGIN_AUTHOR` are **not** part of that API in 4.7.1 — the plugin manager never reads
them (the only `PLUGIN_VERSION` in the tree is defined, never read, by
`quodlibet/ext/songsmenu/tapbpm.py:177`), so this plugin declares the four attributes that
are actually used.

**The event-plugin API.** `EventPlugin` (`quodlibet/plugins/events.py:23-65`) provides the
hooks the adapter implements: `plugin_on_song_started(song)`, `plugin_on_song_ended(song,
stopped)`, `plugin_on_paused()`, `plugin_on_unpaused()`, `plugin_on_seek(song, msec)`
(`:30-51`), plus `enabled()`/`disabled()` for the plugin's own enable state (`:62-65`), and
it sets `PLUGIN_INSTANCE = True` (`:60`), so one instance is created and reused
(`quodlibet/plugins/__init__.py:182-197`). `EventPluginHandler` connects the player's
`song-started`, `song-ended`, `seek`, `paused`, `unpaused` and `error` signals and calls a
hook only when the plugin class actually overrides it
(`quodlibet/plugins/events.py:120-150`); songs arrive wrapped in a `SongWrapper`, which
forwards attribute access, calls and `comma()` to the underlying song
(`quodlibet/util/songwrapper.py:45-80`).

**Reading the playing track.** `app.player` is the live player (`quodlibet/main.py:74`);
`song` is the file being played and `info` is the stream's current song, with the upstream
rule "if you're going to show things, use `.info`" (`quodlibet/player/_base.py:46-62`).
Tags are read as display strings with `song.comma("artist")` / `"title"` / `"album"`
(`quodlibet/formats/_audio.py:762-783`), and the local path is `song("~filename")`
(`:350-403`). A **stream** is a `RemoteFile`, which sets `is_file = False` and stores its
**URI** in `~filename` (`quodlibet/formats/remote.py:17-26` with
`quodlibet/formats/_audio.py:944-975`), so the adapter gates on `AudioFile.is_file`
("Is a real (local) file", `quodlibet/formats/_audio.py:166-167`) and sends `path: ""` for
anything that is not a local file — exactly what protocol §6 asks for.

**Position.** `BasePlayer.get_position()` is documented as *milliseconds*
(`quodlibet/player/_base.py:230-233`) and the GStreamer backend repeats it
(`quodlibet/player/gstbe/player.py:857-861`); `player.seek(pos)` is milliseconds too
(`:920-921`), and the `seek` signal carries the same value once the seek completes
(`:308-310`). There is **no periodic position signal**: the player emits only
`song-started`, `song-ended`, `seek`, `paused`, `unpaused`, `error`
(`quodlibet/player/_base.py:74-81`), and Quod Libet's own periodic source,
`quodlibet.qltk.tracker.TimeTracker`, ticks once a second
(`quodlibet/qltk/tracker.py:20-44`) — coarser than the protocol's 500 ms cadence. The
adapter therefore polls with `GLib.timeout_add(500, …)`, the same mechanism the in-tree
synchronizedlyrics plugin uses for its position-driven updates
(`quodlibet/ext/events/synchronizedlyrics.py:156`).

A note on `song-started`: `_end()` clears the player's current song, announces
`song-ended`, rebuilds the pipeline and only then emits `song-started`
(`quodlibet/player/gstbe/player.py:939-985`), so the position readable at that moment can
still belong to the previous track — the same hazard synchronizedlyrics works around with a
5 ms delay (`quodlibet/ext/events/synchronizedlyrics.py:273-279`). The adapter anchors the
new track at 0 instead and lets the next 500 ms poll re-anchor it; a restored start
position arrives as the player's own `seek` signal.

**Turning the plugin off at runtime.** The plugin manager owns the on/off state:
`PluginManager.enable(plugin, False)` runs the disable path — handlers first, then the
instance's `disabled()`, then the id is dropped from the active set
(`quodlibet/plugins/__init__.py:354-384`) — and `PluginManager.save()` writes that set to
the config (`:340-344`). The Plugins window's own toggle is exactly
`enable(plugin, status)` + `save()` (`quodlibet/qltk/pluginwin.py:550-551`), which is what
the adapter calls when the user closes the lyric window, so the toggle in that window goes
off and nothing respawns. The user re-enables it there (or by restarting Quod Libet with it
still on). The plugin is found through the manager's public `plugins` list, matched on its
own class.

**Preferences pane.** A plugin may define `PluginPreferences(parent)`; the Plugins window
calls it on the instance if it exists and puts the returned widget in the pane
(`quodlibet/qltk/pluginwin.py:344-357`). Options are read and written with
`PluginConfigMixin.config_get`/`config_set`, which key the option as
`<CONFIG_SECTION or PLUGIN_ID.lower().replace(" ", "_")>_<name>` in the `plugins` section
(`quodlibet/plugins/__init__.py:473-506`) — hence `plugins/lxlyrics_app_path`.

**Fullscreen.** Quod Libet's main window is a `quodlibet.qltk.window.Window`, whose F11
handler toggles fullscreen and reads the current state from
`window.get_window().get_state() & Gdk.WindowState.FULLSCREEN`
(`quodlibet/qltk/window.py:147-168`), tracking every change from `window-state-event`
(`:406-407`). The adapter queries the same way for the initial value and connects to the
same signal for transitions.

## Live end-to-end check

With the plugin installed and enabled, play a track in Quod Libet: the lyric window opens,
and `ps` shows exactly one `lx-lyrics-app --player-feed` process whose parent is Quod
Libet. Pause, seek, change tracks, press F11 (the window hides if the app's
`fullscreenHide` setting is on), then close the lyric window: the plugin's toggle in
**Music → Plugins** goes off and the app process is gone.

The adapter was also driven end-to-end outside Quod Libet during development, against a
scratch build of the app (`cmake -S lyrics-app -B /tmp/lx-app-quodlibet -G Ninja
-DCMAKE_BUILD_TYPE=Debug && cmake --build /tmp/lx-app-quodlibet`):

- the real `PluginManager` discovered `lxlyrics` from a plugins folder, the real
  `EventPluginHandler` fed a real `quodlibet.player.nullbe.NullPlayer` through
  `plugin_on_song_started`/`plugin_on_seek`, the real window's fullscreen transition
  produced `set_fullscreen`, and disabling the plugin closed the child's stdin — the app
  logged `feed: connected to "quodlibet" spectrum: false`, then `feed: host closed the pipe,
  exiting` and exited with status 0;
- the feed layer alone (stub Quod Libet, real Gio/GLib) sent `hello` + `set_info` +
  `set_status` to the scratch binary under `QT_QPA_PLATFORM=offscreen` and closed its
  stdin: the app logged the track and exited with status 0.

## Licence

GPL-3.0-only, like this repository's other adapters. Quod Libet itself is
GPL-2.0-or-later (`pyproject.toml:4`; its plugin sources carry the "either version 2 of
the License, or (at your option) any later version" header), whose "or later" clause allows
a GPL-3.0 plugin, so an in-process GPL-3.0-only plugin is fine.
