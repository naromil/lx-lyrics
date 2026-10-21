# LX Lyrics — Fooyin plugin

## What it is

A plugin for [Fooyin Music Player](https://github.com/ludicrousDevelopment/Fooyin) (>= 0.11.1)
that drives the standalone `lyrics-app` display project.

Per `../../docs/protocol.md` (v2) the plugin is an **in-process adapter**:

- It observes playback through Fooyin's API and spawns `lx-lyrics-app --player-feed` as its
  **direct child** (pipes, never detached).
- It pushes playing context — file path, metadata, state, position, and analyser frames when
  Fooyin exposes them — as JSON lines on the child's stdin.
- It **never** reads lyric files, converts encodings, parses LRC, computes line numbers, or
  renders anything — the app owns acquisition, parsing, selection, and rendering.

## How it fits together

```
Fooyin playback (PlayerController) ──► PlayerBridge ──► FeedWriter ──spawn+pipes──► lx-lyrics-app --player-feed
Analyser (VisualisationService) ────► SpectrumSource ──┘                           (acquisition, parsing, rendering)
```

- **PlayerBridge** watches playback and turns track/state changes into `set_info` / `set_status` /
  `set_play` / `set_pause` / `set_stop` lines; `set_info` carries the file path and metadata — the
  app reads the lyrics itself.
- **SpectrumSource** pulls analyser magnitudes, log-scales them into 128 bytes per request.
- **FeedWriter** owns the child process: spawns it, writes the `hello` handshake and every
  host→app line, strictly parses the two app→host actions (`get_analyser_data_array`,
  `close_requested`), and reports the child's exit.
- The lyric window lives and dies with Fooyin: closing the window ends the session, and the app
  quits when its stdin closes (Fooyin exit or toggle off).

The app-side pipeline (acquisition, parsing, line selection, rendering, settings) is entirely the
`lyrics-app` project's job — see `../../lyrics-app/README.md`.

## Requirements

- Fooyin >= 0.11.1, **built with `INSTALL_HEADERS=ON`** so `FooyinConfig.cmake` and the Fooyin
  headers (`/usr/include/fooyin`) are installed. This tree is load-verified against the installed
  Fooyin 0.13.1 (2026-09-27): plugin metadata plus the `Plugin`/`CorePlugin`/`GuiPlugin` interfaces
  resolve via `QPluginLoader`, and the host calls the `PluginConfigGuiPlugin` interface's
  `settingsProvider()` (the Plugins page's Configure button); rebuild against the Fooyin release
  you run, since compatibility is by ABI.
- Qt 6 >= 6.4 — Core, Widgets (FooyinConfig does not propagate Qt modules; declared here).
- A C++23 compiler, CMake >= 3.19, Ninja.
- The `lx-lyrics-app` binary (this repository's `lyrics-app`), on `PATH` or configured via AppPath.
- `lx-lyrics-hotkey` (built and installed next to the app by the `lyrics-app` project when
  `pkg-config gio-2.0` is available) for the global shortcut. Missing is one warning; everything
  except the shortcut still works.

No ICU and no WebSockets here: encoding conversion lives in the app, and the transport is a
private pipe pair.

## Build

```sh
cd plugins/fooyin
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Or use the project's `tools/install.sh --player fooyin` (or with no arguments, which installs every
adapter whose player it finds on the machine) to build and install the app and the plugin, and to point
`AppPath` at the binary without any manual entry.

Artifact: `build/fyplugin_lxlyrics.so`.

The project also builds `hotkey_test`, a hermetic test of the helper transport (`src/hotkey.c`)
that needs no Fooyin headers, player or display server — `/bin/sh` stand-ins speak the fd-3
protocol:

```sh
ctest --test-dir build
# or, standalone: cc -Wall -Wextra -I src src/hotkey.c tests/hotkey_test.c -lpthread
```

## Install

Copy the plugin into Fooyin's plugin directory, then restart Fooyin:

```sh
cp build/fyplugin_lxlyrics.so ~/.local/lib/fooyin/plugins/
# system-wide alternative:
# cp build/fyplugin_lxlyrics.so <prefix>/lib/fooyin/plugins/
```

After restart the plugin appears as **LX Lyrics** (Category: Lyrics) in Fooyin's plugin list, with
**About** and — because it implements `PluginConfigGuiPlugin` — an enabled **Configure** button.

## Use

- **View → Desktop Lyrics** — toggle. Spawns the standalone lyrics app
  (`lx-lyrics-app --player-feed`) as a direct child and pushes the current track right after the
  handshake, so the window shows what is already playing.
- **Settings → Plugins → LX Lyrics → Configure** (Fooyin's per-plugin configuration dialog —
  `PluginConfigGuiPlugin`/`PluginSettingsProvider`; the button is enabled for this plugin):
  - **AppPath** — path to the `lx-lyrics-app` binary. Empty = auto-detect: `PATH` lookup, then the
    plugin's app directory.
  - **RememberState** — remember the desktop lyrics state from the last session (kept under
    `LxLyrics/Enabled`). When on, startup restores the state the previous session ended with;
    when off, desktop lyrics always start off. A fresh install stays off until the user toggles
    **View → Desktop Lyrics** once.
  - **Hotkey** — the *preferred* trigger (`LxLyrics/Hotkey`, default `CTRL+ALT+l`) passed to
    `lx-lyrics-hotkey` when the plugin starts. The desktop owns the real binding — see
    *Global shortcut*.
  - **HotkeyHelper** — path to `lx-lyrics-hotkey` (`LxLyrics/HotkeyHelper`). Empty = next to the
    resolved lyrics app, then `PATH`. Both hotkey values are read when Fooyin starts.
  - **Open lyrics settings** — asks the running app to open its own configuration dialog
    (`open_settings`); no-op while the app is not running.
  - **Restore Defaults** — clears AppPath, re-checks RememberState, restores `CTRL+ALT+l` and
    clears HotkeyHelper in the dialog; the values are written only when the dialog is accepted.
- **Lyrics** — the app reads them itself for the file at `set_info.path`: the same-name `.lrc`
  sidecar and the embedded tag (`LYRICS`, `SYNCEDLYRICS`, `UNSYNCEDLYRICS`, `UNSYNCED LYRICS`,
  plus the `LYRICS:<description>` fallbacks), joined when both exist. Encoding is auto-detected
  inside the app — UTF-8, UTF-16 (BOM), then GB18030→BIG5 via ICU.

## Global shortcut

Fooyin exposes no global-shortcut API to plugins, so the key never passes through the host. The
plugin spawns `lx-lyrics-hotkey` (built from `../../lyrics-app/src/hotkey/`, installed next to
`lx-lyrics-app`) at plugin initialisation and speaks a private one-line protocol with it over
fd 3: `ready <trigger>` once the binding exists, `unavailable <reason>` when it does not, and
`toggle` for every key press.

A `toggle` flips the **View → Desktop Lyrics** checked action on the Qt main thread — exactly
what clicking the menu item does — so it starts a session when none is showing and ends one
when it is. The helper runs for the plugin's whole lifetime, not per session: enabled ≠ showing,
and the key can open a window that is currently hidden. On Fooyin shutdown the helper gets
SIGTERM (SIGKILL after 1 s) and is reaped; a helper that is missing or cannot spawn is one
warning, and everything except the shortcut keeps working.

The helper holds the key through the XDG desktop portal
(`org.freedesktop.portal.GlobalShortcuts`): `CreateSession`, then `BindShortcuts` with
`[LxLyrics] Hotkey` as the `preferred_trigger`. The portal decides the real binding and reports
it back (KDE normalises `CTRL+ALT+l` to `Ctrl+Alt+L`); the plugin logs
`[LX Lyrics] global shortcut bound to <trigger>`.

**The app id comes from the systemd user unit.** The portal accepts a shortcut only for a caller
whose unit is named `app-<app-id>@<uuid>.service`. A player started from the KDE launcher has
one, so the shortcut is associated with Fooyin's own app id. A player started from a terminal
has no unit name, and the portal answers
`org.freedesktop.portal.Error.NotAllowed: An app id is required`; the helper reports
`unavailable ...` on the pipe, exits 0, and the plugin logs it once. That is a normal outcome,
not a failure — everything except the shortcut still works and the menu item remains.

**Re-binding.** `[LxLyrics] Hotkey` is only the *preferred* trigger offered at the next plugin
start (restart Fooyin after changing it; `[LxLyrics] HotkeyHelper` likewise). The binding belongs
to the desktop: open **System Settings → Shortcuts** (KDE), find Fooyin under its own application
group, and change or clear the key there. A combination that is already taken is normally
reassigned by the desktop, which is why the helper reports the trigger it actually got — but
`xdg-desktop-portal-kde` 6.8 can also fail to build its own "shortcut is already in use" message
and then send **no answer at all**. The helper therefore treats a request the desktop leaves
unanswered for 10 s as `unavailable the desktop did not answer the shortcut request`, logs it
once, and exits 0, so nothing hangs.

## Troubleshooting

- **App not found / window never appears**: set **AppPath** to the full path of the `lx-lyrics-app`
  binary instead of relying on auto-detect.
- **No lyrics**: check the track's embedded tags, or drop a same-name `.lrc` beside the audio
  file. The file must be UTF-8 / UTF-16 / GB18030 / BIG5.
- **App aborts**: a protocol violation is fatal by design — the app logs the reason on stderr and
  exits non-zero, and the plugin turns the toggle off. The child's stderr is forwarded into
  Fooyin's log. A crash without a user close is respawned after ~1.5 s while the toggle is on.
- **Always-on-top under Wayland**: X11-native best effort; use KDE window rules
  (System Settings → Window Management → Window Rules) to force e.g. **Keep Above** for the
  lyrics-app window.

## License

GPL-3.0-only — this plugin links Fooyin's GPL-3.0 libraries. The lyrics rendering logic it drives
is ported from lx-music-desktop (Apache-2.0) and lives in the `lyrics-app` project; see the SPDX
header at the top of each source file.
