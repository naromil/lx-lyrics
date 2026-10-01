# lx-lyrics Audacious adapter

A headless [Audacious](https://audacious-media-player.org/) general plugin (`lxlyrics.so`) that
owns the desktop-lyrics session: it spawns the standalone **lx-lyrics** display as its direct child
and feeds it the player state over the **protocol v2 stdin/stdout player feed**
(`../../docs/protocol.md`). The plugin never reads lyrics — the app owns acquisition (sidecar `.lrc` +
embedded tags), parsing and rendering.

Closing the lyric window sends `close_requested`; the adapter ends the session, the app exits on
stdin EOF, and the plugin turns its **own** enable state off — Audacious' Plugins settings page is
the toggle, so the page honestly reads off afterwards.

## Requirements

- **Audacious ≥ 4.6.1**, with its headers. Upstream pins this hard:
  - The plugin header carries the API version it was compiled against
    (`const int version = _AUD_PLUGIN_VERSION;`, `src/libaudcore/plugin.h:127`) and 4.6.1 sets both
    bounds to 49 (`_AUD_PLUGIN_VERSION_MIN`/`_AUD_PLUGIN_VERSION`, `plugin.h:50-51`), where 4.6 had
    48 (tag `audacious-4.6`, same two lines). The loader rejects anything outside that window
    (`plugin-load.cc:85-91`, *"… is not compatible with this version of Audacious"*), so **one
    binary cannot serve both 4.6 and 4.6.1** — this adapter is built against 4.6.1 (the current
    release) and therefore declares **4.6.1** as its floor.
  - The headers install as `libaudcore/*.h` under the prefix
    (`install_headers(libaudcore_headers, subdir: 'libaudcore')`, `src/libaudcore/meson.build:141`),
    and `pkg-config audacious` names the prefix (`audacious_include_dir`, its `audacious.pc`).
- **libaudcore**: the module links `-laudcore`, exactly like upstream's plugins do through their
  `audacious_dep` (`audacious-plugins/src/songchange/meson.build`). Nothing else: **no libaudgui,
  no libaudqt, no GTK/Qt** — the plugin has no window and no menu item, and its one preferences
  page is plain declarative data (`PreferencesWidget`/`PluginPreferences`) that the host's own
  frontend renders (see *Configuration*).
- **C++17** for the glue and **C99** for the transport: libaudcore's plugin API is a C++ class
  hierarchy (`plugin.h:119` `class LIBAUDCORE_PUBLIC Plugin`, `:498`
  `class LIBAUDCORE_PUBLIC GeneralPlugin`), and upstream builds with `cpp_std=gnu++17`
  (`meson.build:6`).
- `lx-lyrics-app` installed (e.g. by `../../tools/install.sh`), findable on `$PATH`.

## Build

CMake (headers via pkg-config, or an explicit include dir):

```sh
cmake -B build -G Ninja                                        # pkg-config audacious
cmake -B build -G Ninja -DAUDACIOUS_INCLUDE_DIR=/path/to/include
cmake --build build          # -> build/lxlyrics.so
ctest --test-dir build       # hermetic protocol tests, no Audacious needed
```

A bare `cmake` configures **Debug** (the same default the app project uses); pass
`-DCMAKE_BUILD_TYPE=Release` for the plugin you install. The configure step **requires** the
Audacious headers — a bare `cmake -B build -G Ninja` FATAL_ERRORs when they are missing — while the
`feed_test` target builds only `feed.c` + `json.c`, so `ctest` runs with no Audacious installed.

Or directly, with the same four sources:

```sh
INC=$(pkg-config --variable=audacious_include_dir audacious)   # or a checkout's src/
cc  -std=c99  -fPIC -O2 -I"$INC" -Isrc -c src/feed.c src/json.c
c++ -std=gnu++17 -fPIC -O2 -I"$INC" -Isrc -c src/plugin.cc src/spectrum.cc
c++ -shared -o lxlyrics.so feed.o json.o plugin.o spectrum.o $(pkg-config --libs audacious) -lpthread -lm
```

The module name matters: the loader looks up `aud_plugin_instance` inside every scanned file
(`plugin-load.cc:75`) and takes the file **basename** as the plugin id
(`plugin-registry.cc:109-118`), while only files ending in `PLUGIN_SUFFIX` are scanned at all
(`plugin-load.cc:134`; `.so` on Linux, `meson.build:121`). Upstream builds its plugins the same way,
with `name_prefix: ''`.

## Install

Audacious scans **`<PluginDir>/General/`** for `*.so` (`plugin-load.cc:35-37` lists the category
subdirectories, `:155-164` scans them), where `PluginDir` is the compiled-in `INSTALL_PLUGINDIR` =
`<prefix>/lib/audacious` (`meson.build:159`, `:172`; `runtime.cc:188`), **relocated to match the
running executable** (`runtime.cc:198-243`) so a locally built Audacious finds its own tree.

```sh
# a distro Audacious (/usr/bin/audacious) — needs root, or a distro package:
install -m 755 build/lxlyrics.so /usr/lib/audacious/General/

# an Audacious built into <prefix> — no root needed:
install -m 755 build/lxlyrics.so <prefix>/lib/audacious/General/
```

There is **no per-user plugin directory** and no command-line or environment override for it:
`AudPath::PluginDir` is only ever the install dir above, and Audacious' option table
(`src/audacious/main.cc:74-98`) has no such flag. Audacious has to be **restarted** to dlopen the
plugin (`File → Settings... → Plugins` lists it afterwards).

`../../tools/install.sh --player audacious` builds the module, installs it into the detected
plugin directory and writes `[lx-lyrics] app_path`; because that directory is root-owned for a
distro Audacious, it prints the exact `sudo install` command instead of installing and exits
non-zero.

## Use

1. Open the Settings dialog's Plugins page — **File → Settings... → Plugins** (Qt) or **File →
   Settings → Plugins** (GTK); `Ctrl+P` opens the dialog — and enable **LX Lyrics**. That *is* the
   toggle: Audacious starts every enabled general plugin at every player start and calls its
   `init()`/`cleanup()` on enable/disable (`plugin-init.cc:31-43`, `:160-176`, `:285-308`, reached
   from `aud_run()` at `runtime.cc:339-357`). The plugin adds no menu item — the host's Plugins
   page is the enable/disable control, the same decision the Rhythmbox adapter documents for its
   host — and its one settings page (the app path, the remember-state checkbox and the button that
   raises the app's own settings dialog) is the host's own dialog, reached from that page (see
   *Configuration*). The row's separate information icon opens the plugin's own *About* text.
2. Start playback: enabling the plugin during playback shows the current track immediately (the
   full `set_info` + state snapshot is pushed right after the spawn, protocol §3/§5).
3. Closing the lyric window ends the session and turns the plugin's enable state off, so the
   Plugins page reads off and nothing is respawned (§4).
4. If `lx-lyrics-app` cannot be found, or the app dies or breaks the protocol, the plugin logs it
   loudly and turns its own enable state off rather than claiming a session that does not exist
   (§7). Nothing is ever respawned automatically; re-enable it in the Plugins page to retry.

## Configuration

The plugin's settings page is its 4th `PluginInfo` field — a declarative
`const PluginPreferences*` (libaudcore's `preferences.h`) that both frontends build with no toolkit
code here: one `WidgetLabel`, one `WidgetEntry("Application:", WidgetString("lx-lyrics",
"app_path"))`, one `WidgetCheck("Remember the desktop lyrics state from the last session",
WidgetBool("lx-lyrics", "remember_state"))`, and one `WidgetButton("Open lyrics settings")`. The
entry writes the key as the user types (`WidgetConfig::set_string()` → `aud_set_str()`) and it is
the same `[lx-lyrics] app_path` `resolve_app_path()` reads; an empty value is the documented
"search `$PATH`" state. The checkbox is the same kind of binding over `aud_get_bool()` /
`aud_set_bool()` (`preferences.cc:25-48`), and the plugin registers `remember_state=TRUE` as the
section default at `init()` (`aud_config_set_defaults()`, read back through `aud_get_str()`'s
defaults fallback at `config.cc:402-412`), so the box reads checked on an untouched config.

What the checkbox means here (it mirrors Fooyin's `LxLyrics/RememberState`, and this is the one
place the host's model differs): Audacious' Plugins page toggle **is** this plugin's session switch,
so "restore the last state" and "the plugin is still enabled at the next player start" are the same
question. What identifies that restore is the interface, which the host starts *after* the general
plugins — `start_plugins_two()` runs Vis, General, Iface in that order (`plugin-init.cc:205-210`) —
so no interface is current in that one `init()`, while the Plugins page (and `audtool`, and D-Bus)
can only ever enable LX Lyrics with one already running. A session the user asks for is therefore
never gated, not even the very first click after a start in which the plugin was disabled.

- **Checked** (the default): nothing extra happens — the host's own registry keeps the enable state
  and a session that was running comes back at the next start.
- **Unchecked**: that restore turns the plugin's own enable state off *instead of* starting a
  session, so the next player start begins disabled (the same "no session ⇒ toggle off" shape the
  app-not-found path uses, §4/§7), the row's checkbox reads off, and the lyrics window does not
  come back on its own — not after a clean exit, and not after a run that was killed outright
  (its saved enable state just makes the next start refuse again, before anything is spawned).
  The user re-enables LX Lyrics in the Plugins page when they want lyrics again.

A `--headless` run starts no interface at all, so there is nothing to tell the restore apart by
there: the check is skipped (`aud_get_headless_mode()`) and the box has no effect.

The button is the route to the app's **own** settings: the app owns its 37 config keys and renders
them in its dialog, so the page does not duplicate them — it sends `{"v":2,"action":"open_settings"}`
(protocol §5) instead, the same call the host makes from the feed, leaving the dialog (Ctrl+, /
control-bar gear) to the app. `WidgetButton` carries the callback declaratively
(`WidgetVButton{...}`, `preferences.h:299`; upstream uses it the same way for "Restart in GTK
mode"), and both frontends invoke it on click — Qt through a `QPushButton`
(`ButtonWidget` → `clicked` → `data.button.callback`, `libaudqt/prefs-widget-qt.cc:55-63`), the
libaudgui frontends (GTK and Audacious' skins interface, which delegates its Preferences window to
`audgui_show_prefs_window()`, `skins/plugin.cc:84-85`) through
`audgui_button_new(label, icon, callback)` (`libaudgui/prefs-widget.cc:544-547`,
`libaudgui/util.cc:161-176`; a null icon yields a plain text button). The callback runs on the
program's main thread, where the session pointer is written, and it is a no-op while no session is
running — so it needs no enable/disable state of its own.

The row also carries an **information button**: `PluginInfo.about` is the non-null text that gates
it (`aud_plugin_has_about()` is literally `about != nullptr`), painted as a "dialog-information"
icon in the About column by the Qt model (`prefs-pluginlist-model.cc:159-163`, click →
`audqt::plugin_about()`, `prefs-plugin.cc:41-61`) and added as an `_About` button by the GTK/skins
view (`plugin-view.cc:202-207`, `:243-247`). Both open a plain message dialog titled *About LX
Lyrics* with that text, which is why it is written as plain text (`plugin.cc`'s `lx_about`).

Both frontends offer the page only while the plugin is **enabled** —
`aud_plugin_has_configure()` is literally `(bool)info.prefs`, and the UI additionally requires the
plugin's enable state:

- **Qt** — **File → Settings... → Plugins →** the gear icon on the **LX Lyrics** row;
- **GTK** (and the skins interface's Preferences window) — **File → Settings → Plugins →** select
  the **LX Lyrics** row → the **Settings** button.

The host's Plugins page stays the enable/disable control; the settings page edits the app path and
the restore policy, never the app's own keys.

The keys themselves, in `$XDG_CONFIG_HOME/audacious/config` (`config.cc:259`, INI sections at
`config.cc:245`):

| Section / key | Default | Meaning |
|---|---|---|
| `[lx-lyrics] app_path` | *(empty)* | Path to `lx-lyrics-app`; empty means "search `$PATH`". |
| `[lx-lyrics] remember_state` | `TRUE` | Restore the last session at the next player start; `FALSE` makes the plugin drop its own enable state on that restore instead. |

## How the session works

- **Feed** (`src/feed.c`, pure POSIX C — it never includes an Audacious header): two `pipe2()`
  pipes plus `fork()`/`execv()` of `<app_path> --player-feed`, the parent closing the child's ends.
  The app's **stderr is inherited, not piped**, so its log lines land on the player's own stderr (a
  terminal or journal) rather than in a pipe the plugin drains. The first line is
  `{"v":2,"action":"hello","host":"audacious","spectrum":true}`; every write goes through one mutex
  as `write()` + `'\n'`.
- **Reader thread**: blocks in `poll()` on the app's stdout with a 500 ms timeout. On timeout it
  emits the periodic `set_status` while playing; on data it parses `get_analyser_data_array` and
  `close_requested`, tolerating/skipping non-JSON lines loudly (a stray Qt stdout write must not
  break the feed). A *well-formed* v2 violation is a protocol error instead (§7): a
  missing/unsupported `v`, an unknown action or a line over 1 MiB is logged loudly and ends the
  session — the app's stdin is closed so it quits on EOF, the plugin's enable state goes off, and
  nothing is respawned. Every write end is non-blocking with a bounded wait, so a stalled app can
  never wedge Audacious, and `SIGPIPE` is blocked around writes instead of being ignored
  process-wide. The feed object is reference-counted (`lx_feed_acquire`/`lx_feed_release`), because
  the app's analyser request is answered from the reader thread while the main thread writes.
- **Message mapping** (`src/plugin.cc`). Every hook is delivered on the program's **main thread**
  (see below), so no marshalling is needed for them:

  | Audacious hook / event | Feed line |
  |---|---|
  | `"playback begin"` (`playlist.cc:207`, also on a playing-position change, `:504`/`:1032`) | `set_info` + `set_play(ms)` (or `set_pause` when it starts paused) |
  | `"playback stop"` (`playlist.cc:209`, `:1037`) | `set_stop` |
  | `"playback pause"` / `"playback unpause"` (`playback.cc:484`) | `set_pause` / `set_play(ms)` |
  | `"playback seek"` (`playback.cc:268`, and the A-B/repeat loop at `:326`) | `set_play(ms)`, plus `set_pause` when the player is paused |
  | `"tuple change"` (`playback.cc:146`) | a fresh `set_info` for the playing track |
  | (spawn, §3) | `set_info` + state snapshot right after `hello` |
  | the settings page's **Open lyrics settings** button | `open_settings` (no session → no-op) |

  A seek needs no heuristic: Audacious reports the event (`playback.cc:268`), unlike Rhythmbox. The
  seek is sent as `set_play` + `set_pause` while paused, because the app resumes its lyric timer on
  `set_play` (§5) and `LyricPlayer::play()` first resyncs the clock and selects the line at the seek
  target, after which `pause()` stops the timer again.
- **Threading**. `drct.h:29` warns that the `aud_drct_*` group is *"not thread safe"*, and the one
  getter this adapter needs for `path`, `aud_drct_get_filename()`, reaches the playlist lock
  (`drct.cc:67-71` → `Playlist::playing_playlist()`, `entry_filename()`). So **every** host call
  happens on the main thread — `init()`, the hook handlers above and a `TimerRate::Hz4` timer
  (`hook.h:56-93`, 250 ms, `timer.cc:27`) — which refreshes a mutex-guarded snapshot
  (state, position, sample rate, `path`, artist/title/album). The feed's reader thread only *reads*
  that snapshot, including for its 500 ms `set_status` clock; it never calls an `aud_*` function.
  This is also what upstream documents for the plugin type: *"Single-thread plugins: visualization,
  general, and interface. Functions provided by these plugins will only be called from the main
  thread"* (`plugin.h:78-79`).
  The session-end notifications from the reader thread hop back to the main thread through a
  `QueuedFunc` (`mainloop.h:29-45`), the host's own cross-thread mechanism — `event_queue()` is
  built on it (`eventqueue.cc:77`) and its registry is mutex-guarded (`mainloop.cc:39-45`).
- **`path`** is the playing entry's URI converted with `uri_to_filename(uri, false)`
  (`audstrings.h:95`), which yields the local path for a `file://` URI and *nothing* for any other
  scheme (`audstrings.cc:622-631`) — exactly the `""` of protocol §6 for a stream or a CD track.
- **Spectrum** (`src/spectrum.cc`, protocol §5). Audacious exposes its analyser to plugins: a
  general plugin may subclass the public `Visualizer` class (`visualizer.h:25-48`) and register it
  with the exported `aud_visualizer_add()` (`interface.h:53`). It asks for the `Freq` mask, so
  `render_freq()` hands it the 256 positive frequencies of the host's 512-sample FFT
  (`fft.cc:101-104`; "intensity of frequencies 1/512 … 256/512 of sample rate",
  `visualizer.h:38`). The frame pointer is only valid during the call, so the main-thread callback
  only copies it; the reader thread band-averages it into 128 log-spaced bands from 40 Hz to Nyquist
  and applies `byte = clamp(round(255 * log10(1 + magnitude * 255) / log10(256)), 0, 255)`.
  `hello.spectrum` is `true` because this registration is real API, not a guess.
  Two consequences are worth knowing: registering a visualizer also switches the host's vis runner
  on for the whole session (`visualization.cc:34-41` → `vis-runner.cc:212-217`), and frames arrive
  from the host's 30 Hz vis timer (`vis-runner.cc:128`) on the main thread.
- **Not sent**: `set_lyric`/`set_offset` (this adapter has no lyric text and no offset control),
  `set_playbackRate` (libaudcore's public API has no playback
  rate at all — upstream's Speed and Pitch plugin is an *effect* that resamples, not a rate the host
  can report), and **`set_fullscreen`**: Audacious exposes no queryable fullscreen state —
  `libaudcore`'s public headers and the whole `audacious-plugins` tree contain no fullscreen API or
  hook — so the adapter omits the action instead of inventing one.

## Tests

`tests/feed_test.c` is hermetic: it compiles `feed.c` + `json.c`, spawns a POSIX shell stub as the
"app" (which logs every received line to `$LX_STUB_LOG` and prints the app→host lines itself) and
asserts the exact host→app bytes (`hello`, the snapshot, `set_info`, `set_status`, `set_play`,
`set_pause`, `open_settings`, `set_stop`), the periodic `set_status` cadence, the `close_requested`
read-back followed by the session ending with the child getting EOF and no respawn, the non-JSON
tolerance (including that the log carries only a bounded printable prefix), the 128-byte spectrum
rule, the no-session/exec-failure paths, each §7 protocol-error case (unsupported `v`, unknown
action, over-1 MiB line → reported once and the session ends, with the app quitting on the EOF it
was sent), that a failed write closes the app's stdin without leaking a descriptor, and that a
writer thread keeps working (as a no-op) while another thread stops the session. No Audacious
headers, player or display server needed:

```sh
cmake -B build -G Ninja -DAUDACIOUS_INCLUDE_DIR=/path/to/include && cmake --build build
ctest --test-dir build --output-on-failure
```

The concurrency case is worth running under sanitizers:
`-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"` plus the matching
`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS`; it reports a heap-use-after-free if a session
object is freed while a writer still holds a reference to it.

## Live end-to-end

The normal check needs an Audacious ≥ 4.6.1 with the plugin installed (`command -v audacious`),
`lx-lyrics-app` on `$PATH`, and a track with lyrics:

1. Start Audacious, **File → Settings... → Plugins → LX Lyrics** on, then play a track. The lyric
   window appears and follows the track, seeks/pauses/stops follow the player, and the visualizer moves
   when `desktopLyric.audioVisualization` is enabled in the app.
2. The app's own log lines arrive on Audacious' stderr (`feed: connected to "audacious"
   spectrum: true`, `feed: track "…"`), because the child's stderr is inherited.
3. Open the LX Lyrics row's settings page (**File → Settings... → Plugins → General →** the gear
   icon on **LX Lyrics**) — it holds the app path, the **Remember the desktop lyrics state from the
   last session** checkbox and **Open lyrics settings**, which raises the running app's own
   *Desktop Lyric Settings* dialog (`open_settings` on the wire; a silent no-op with no session).
   The row's separate information icon opens *About LX Lyrics* with the plugin text
   (`PluginInfo.about`).
4. Uncheck that checkbox and quit the player while the lyric window is up: the next start has no
   window and the row reads off — the restore is refused before anything is spawned, which is how
   "don't remember" is expressed on this host (see *Configuration*). Enabling the row by hand then
   starts a session as usual, remember-state or not.
5. Close the lyric window: the adapter logs `lx-lyrics: ending the session (the lyric window was
   closed)`, the Plugins page flips off, and no window comes back until you re-enable it.

Without touching the system installation (no root) the same thing can be driven end to end: copy
`/usr/bin/audacious` and `/usr/lib/audacious/` into a scratch prefix, drop `lxlyrics.so` into its
`General/`, mark it enabled in an isolated `$XDG_CONFIG_HOME/audacious/plugin-registry`
(`format 11` + the plugin block with `enabled 1`, `plugin-registry.cc:42`/`:320-323`), and run it
headless with a `$PATH` stub (or the real app) as `lx-lyrics-app`:

```sh
XDG_CONFIG_HOME=/tmp/aud-conf PATH=/tmp/aud-bin:$PATH QT_QPA_PLATFORM=offscreen \
  timeout 30 /tmp/aud-prefix/bin/audacious --headless -q -p track.wav -V
```

That is how this adapter was verified: the stub logged
`{"v":2,"action":"hello","host":"audacious","spectrum":true}`, the initial snapshot, `set_info`
with the playing file's local path, `set_play`, nine `set_status` lines at ~500 ms with an
advancing `played_time`, `set_stop` at the end of the track, then `EOF_SEEN` when the player shut
down; 26 `get_analyser_data_array` requests were answered with 128-byte frames whose peak landed on
the bands covering the 440 Hz test tone; with the **real** app as the child, the app logged
`feed: connected to "audacious" spectrum: true` / `feed: track "…"` / `feed: host closed the pipe,
exiting` and exited 0; and a stub that sent `close_requested` (or a `{"v":3,…}` violation) made the
adapter log the reason, close the child's stdin and persist `enabled 0` for its own plugin entry.

The settings page was verified the same way, with the Qt frontend instead of `--headless` (a
virtual display is enough: `QT_QPA_PLATFORM=xcb DISPLAY=:N … audacious -Q -m`): the **General → LX
Lyrics** gear opened a *LX Lyrics Settings* window holding the label, the `Application:` entry, the
**Remember the desktop lyrics state from the last session** checkbox and the **Open lyrics
settings** button, and clicking the button made the real app open its *Desktop Lyric Settings*
window — i.e. the same thing `open_settings` does from the feed. The row's information icon opened
*About LX Lyrics* with the `lx_about` text, and the registry written for the plugin carries
`about 1` (the host records `PluginInfo.about != nullptr`), which is what paints that icon.

The remember-state checkbox was driven through the same GUI: on an untouched config it reads
**checked** (`aud_config_set_defaults` registers `TRUE`), unchecking it wrote
`[lx-lyrics] remember_state=FALSE` into `audacious/config`, and with that key set the next GUI start
logged only `lx-lyrics: ending the session (the last session is not remembered)` — no session, no
child, `enabled 0` saved for the plugin's own registry entry — while unchecking it in a run whose
plugin was **disabled at startup** and then enabling it by hand *did* start a session (a user enable
is never gated), and checking the box again removed the key from the file — `config_save()` writes
nothing for a value that equals the registered default, and the box still reads checked.

## License

GPL-3.0-only (see the SPDX headers); it is an Audacious plugin and links against Audacious'
libaudcore.
