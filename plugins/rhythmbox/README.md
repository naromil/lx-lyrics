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
cp -r plugins/rhythmbox ~/.local/share/rhythmbox/plugins/lxlyrics
```

Rhythmbox scans `$XDG_DATA_HOME/rhythmbox/plugins/<name>/` (`~/.local/share` by default)
for per-user plugins, so no root is needed. `../../tools/install.sh --player rhythmbox`
copies those two files and writes `app-path` into the config file below for you. Then:

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

- Rhythmbox with Python plugin support (`plugins_python`): PyGObject, `libpeas`/`libpeas-gtk`
  **and the libpeas Python 3 plugin loader** — the module
  `<libdir>/libpeas-1.0/loaders/libpython3loader.so`, whose loader id (`python3`) is the one
  `shell/rb-shell.c` enables unconditionally. `libpeas` alone is not enough: the loaders are
  separate modules and a distribution may build libpeas without the Python one. **Arch's
  `libpeas` is such a build** — since `1.36.0-7` its PKGBUILD passes `-D python3=false`, so
  `/usr/lib/libpeas-1.0/loaders/` holds only `liblua51loader.so` — and **no Arch package
  provides it for libpeas 1.x** (`libpeas-2`'s `libpeas-2/loaders/libpythonloader.so` is a
  loader for `libpeas-2.so.0` with loader id `python`; Rhythmbox links `libpeas-1.0.so.1`, so
  it is not a substitute). There, *every* Python Rhythmbox plugin (this one included) fails,
  whatever its own metadata says; libpeas logs

  ```
  libpeas-WARNING **: Failed to load module 'python3loader': /usr/lib/libpeas-1.0/loaders/libpython3loader.so: cannot open shared object file: No such file or directory
  libpeas-WARNING **: Could not load plugin loader 'python3'
  ```

  and Rhythmbox's Plugins page marks the plugin as not loadable. Upstream libpeas 1.38.1
  still builds the loader by default (`option('python3', value: true)` in its
  `meson_options.txt`), so a distribution only has it if it does not switch it off. Where it
  is missing, build it (see below) — do not look for a code workaround here.
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

### Provisioning the Python loader where it is missing

The loader is a shared module libpeas `dlopen`s lazily the first time a `Loader=python3`
plugin is loaded. It must be built against the *installed* PyGObject and Python, from the
libpeas source matching the installed `libpeas-1.0.so.1` (1.38.1 on Arch), and it needs no
root to build:

```sh
git clone --depth 1 --branch libpeas-1.38.1 https://gitlab.gnome.org/GNOME/libpeas.git
cd libpeas
meson setup build -Dpython3=true -Dlua51=false -Ddemos=false -Dglade_catalog=false -Dvapi=false
meson compile -C build        # -> build/loaders/python3/libpython3loader.so
```

Then either install it system-wide, which needs root and afterwards nothing else:

```sh
sudo install -Dm755 build/loaders/python3/libpython3loader.so \
    /usr/lib/libpeas-1.0/loaders/libpython3loader.so
```

or keep it in your home. `PEAS_PLUGIN_LOADERS_DIR` *replaces* libpeas's built-in loaders
directory and is searched as `<dir>/<loader id>/`, so the module goes one level deeper:

```sh
install -Dm755 build/loaders/python3/libpython3loader.so \
    ~/.local/share/libpeas-1.0/loaders/python3/libpython3loader.so
```

and `PEAS_PLUGIN_LOADERS_DIR=$HOME/.local/share/libpeas-1.0/loaders` has to be in the
environment Rhythmbox starts with (for a systemd session,
`~/.config/environment.d/libpeas.conf` with `PEAS_PLUGIN_LOADERS_DIR=…`, then log back in).
Because that variable replaces the whole directory, any other loader in use (`lua5.1`) must
live under it too.

### Why `lxlyrics.plugin` declares no `Depends=`

The adapter imports nothing from Rhythmbox's shared Python code (the `rb` plugin's module,
`/usr/lib/rhythmbox/plugins/rb/rb.py`): it uses only the `RB` typelib, which comes from
`librhythmbox-core` and is already required by Rhythmbox's `construct_plugins()` before any
plugin loads. Rhythmbox's own Python plugins declare `Depends=rb` exactly when they
`import rb` (`lyrics`, `webremote` and the rest do; `pythonconsole`, which does not,
declares none).

Declaring it without needing it is worse than useless:

- with the Python loader missing, libpeas reports
  `Dependency “Shared plugin code” failed to load` — the display name it took from
  `rb.plugin`'s `Name=` key — instead of the actual cause,
  `Plugin loader “python3” was not found`;
- the plugin also refuses to load wherever `rb` cannot, even though it does not depend on it.

(`rb` is only loaded on demand: `shell/rb-shell.c` skips the builtin `rb` module unless some
plugin depends on it.)

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

libpeas's own `libpeas-WARNING **: Could not load plugin loader 'python3'` in that log is
not the adapter's line: it means the libpeas Python loader is missing, so no Python plugin
in Rhythmbox can load — see Requirements.

## Tests

Hermetic — no Rhythmbox, GTK, PyGObject or display server needed. The adapter is imported
against stub `gi.repository` namespaces and a stub `RBShellPlayer`, and the tests assert
the exact JSON lines emitted for scripted player events (handshake, track change, pause,
500 ms `set_status` coalescing, seek discontinuity, `close_requested`), plus the process
lifecycle and the key-file round-trip.

```sh
python3 -m unittest discover -s plugins/rhythmbox/tests -v
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
