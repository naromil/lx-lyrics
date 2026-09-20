# LX Lyrics — Rhythmbox plugin

A Rhythmbox (libpeas/Python) adapter for the standalone **lx-lyrics** desktop lyrics
display. The plugin observes `RBShellPlayer`, spawns `lx-lyrics-app --player-feed` as its
**direct child** and streams newline-delimited v2 JSON to the child's stdin. The app owns
everything else: it reads the sidecar/embedded lyrics of the file at `path`, parses them,
and renders them.

The lyric window therefore lives and dies with Rhythmbox: closing the window ends the
session, and the app quits when its stdin closes.

## Install

```sh
mkdir -p ~/.local/share/rhythmbox/plugins
cp -r rhythmbox-plugin ~/.local/share/rhythmbox/plugins/lxlyrics
```

Rhythmbox scans `$XDG_DATA_HOME/rhythmbox/plugins/<name>/` (`~/.local/share` by default)
for per-user plugins, so no root is needed. Then:

1. Restart Rhythmbox and open **Tools → Plugins** (or **Edit → Plugins**) and make sure
   **LX Lyrics** is enabled — a fresh install ships with `InitiallyEnabled=true`, so it
   usually already is.
2. Start the lyrics display with **View → LX Lyrics**. This is the session toggle; the
   plugin's own enable/disable stays Rhythmbox's Plugins page. Nothing is spawned until
   you toggle it, so a fresh install never opens a lyric window on its own.
3. If `lx-lyrics-app` is not on `PATH`, set its path in **Tools → Plugins → LX Lyrics →
   Preferences**. The app is always started as `<executable> --player-feed`.

Closing the lyric window (its control bar or the WM close button) turns the session off
and remembers that, so it does not come back on the next start. Toggling **View → LX
Lyrics** off does the same, and so does the app exiting on its own. Disabling the
*plugin* stops the session but remembers whether it was running.

## Requirements

- Rhythmbox with Python plugin support (`plugins_python`), i.e. PyGObject plus
  `libpeas`/`libpeas-gtk`.
- The lx-lyrics display app (this repository's `lyrics-app`), built and installed.

PyGObject version notes (these are Rhythmbox's own build constraints, quoted from its
`meson.build`):

- **PyGObject 3.52 does not work at all**: `error('rhythmbox cannot be used with pygobject
  3.52 due to girepository clashes')`.
- **PyGObject ≥ 3.53 requires girepository 2.0** (`girepository-2.0`, provided by GLib ≥
  2.80 and libpeas > 1.37); otherwise Rhythmbox refuses the combination with `error('cannot
  mix girepository 1.0 (via libpeas) and 2.0 (via pygobject)')`.

Rhythmbox has **no analyser API** (its Visualizer plugin was removed in 2017), so the
handshake declares `spectrum: false` and the app never requests spectrum frames.

## Configuration

Session state lives in a plain key file — deliberately not `Gio.Settings`, which would
mean shipping and compiling a GSettings schema for a user-level plugin:

`$XDG_CONFIG_HOME/lx-lyrics/rhythmbox.conf`

```ini
[lx-lyrics]
enabled=true
app-path=/usr/local/bin/lx-lyrics-app
```

`enabled` is the remembered session state; `app-path` is the executable spawned with
`--player-feed`. Both are written by the plugin (the preferences pane edits `app-path`,
the View menu toggle and a closed lyric window edit `enabled`).

## Debugging

Rhythmbox redirects a Python plugin's `print` output into its own debug logging, so run
Rhythmbox from a terminal with the plugin name as the filter:

```sh
rhythmbox -D lxlyrics
```

That shows the adapter's own lines (spawn failures, stray non-JSON output from the app, a
protocol error that ends the session, and the `close_requested`/exit transitions). Use
plain `-d` only if you want all of Rhythmbox's debug output.

## Tests

Hermetic — no Rhythmbox, GTK, PyGObject or display server needed. The adapter is imported
against stub `gi.repository` namespaces and a stub `RBShellPlayer`, and the tests assert
the exact JSON lines emitted for scripted player events (handshake, track change, pause,
500 ms `set_status` coalescing, seek discontinuity, `close_requested`), plus the process
lifecycle and the key-file round-trip.

```sh
python3 -m unittest discover -s rhythmbox-plugin/tests -v
```

(From inside this directory, `python3 -m unittest discover -s tests -v` is equivalent.)

## Wire contract

One JSON object per UTF-8 line. The first line written is the handshake:

```json
{"v":2,"action":"hello","host":"rhythmbox","spectrum":false}
```

Host → app: `set_info` (path, singer, name, album, empty lyric fields — the app reads the
file at `path`), `set_status`, `set_play`, `set_pause`, `set_stop`. App → host:
`close_requested` only — any other well-formed line (a `v` that is not 2, an unknown
action, an analyser request this host never declared, a line over 1 MiB) is a protocol
error that ends the session, while non-JSON stdout noise is skipped. See `docs/protocol.md`
for the full v2 contract.

## Licence

GPL-3.0-only, like this repository's other adapters. Rhythmbox itself is GPL-2.0-or-later,
whose "or later" clause allows a GPL-3.0 plugin, so an in-process GPL-3.0-only plugin is
fine.
