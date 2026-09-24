# Fooyin GUI rig — runbook

A throwaway, fully isolated X session running real Fooyin + the real `lx-lyrics-app` child,
for the `plugins/fooyin` bugs that only exist at runtime: plugin load, the `Ctrl+I`
Desktop Lyrics toggle, the app's spawn/exit cycle, window mapping/positioning, state
restored wrong on the next session, and the player-quit teardown path.

## Why the `feed` suite is not enough

`ctest --test-dir lyrics-app/build -R feed` compiles both halves of the wire (the app's
`FeedReader` and the plugin's Fooyin-free `feedwriter.cpp`) and asserts the JSON protocol —
headless, with no Fooyin, no plugin load, no X server and no window. It cannot see a plugin
that never loaded, a window that never mapped, a toggle that did nothing, or a window that
comes back at the wrong place. That is this rig's surface. Verification here is
**screenshot + log grep, read by a human or model** — no pixel diffing, no OCR, no
assertions, no CI.

## Interface

```
tools/gui-test/rig.sh start|stop|status|restart [--rig-root DIR] [--app PATH] [--track PATH] [--force] [--purge]
tools/gui-test/rig.sh fooyin <fooyin args…>   # run the fooyin CLI against the rig's instance
tools/gui-test/rig.sh key <xdotool args…>     # focus the main window and run xdotool key …
tools/gui-test/seed-config.py [--rig-root DIR] [--app PATH] [--track PATH]
tools/gui-test/appwrap.py [--app PATH] [--rig-root DIR] [--feed-log PATH] [--player-feed]
```

Every entry point takes `--help`. `--purge` only means something with `stop`/`restart`; with
`start`, `status`, `fooyin` or `key` it warns and is ignored (`--purge only means something
with stop or restart; ignoring it here`) — the same rule for all four, not just the two the
old revision covered. Everything after `fooyin` or `key` goes to fooyin/`xdotool` itself, so
rig options must come first: `tools/gui-test/rig.sh --rig-root DIR fooyin -p file`. A rig
option placed *after* the subcommand is refused by name rather than passed through —
`error: '--app' is a rig.sh option, not a 'fooyin' argument: options must come before the
subcommand (e.g. rig.sh --rig-root DIR fooyin ...)`. `seed-config.py` rarely needs to be run
by hand: `start` runs it on every start that actually builds the rig. When the rig already
looks alive, `start` warns, prints `status` and returns **without seeding** (use `restart` to
re-seed) — and it is idempotent anyway (a file is rewritten only when its content differs, so
unchanged files keep their mtime).

The rig root is `${LX_RIG_ROOT:-/tmp/lx-lyrics-gui-rig}` and **must be an absolute path in all
three scripts**: `--rig-root DIR` overrides the default, and both it and the `LX_RIG_ROOT`
default are rejected when relative — `error: the rig root must be an absolute path (got '…')`
from `rig.sh` (at parse time, before any command runs) and from `seed-config.py`/`appwrap.py`
too, so a relative root never resolves silently against the cwd. `rig.sh` additionally refuses
`/`, a top-level directory (`/tmp` itself — the default root is `/tmp/<name>`), `$HOME` or the
repository, a directory that *contains* either of them, a directory *inside* the repository
(never legitimate: a marker there would make a later `stop --purge` delete the sources), and
(for `start`/`--purge`) a directory that contains another rig root. That check is one shared
validator every subcommand runs before anything is created, signalled or removed. On top of it,
`start` refuses a path that already exists, is not a rig root and is not empty (`refusing to
start at …: the directory already exists, is not a rig root and is not empty; pick a fresh path,
or point --rig-root at an existing rig root`) — a non-existent path, an empty directory and an
existing rig root are all fine. Two more takeover refusals guard the unverifiable and the racing
case: a directory whose contents **cannot be listed** is refused (`refusing to start at …: cannot
read the directory to verify it is empty` — a mode-0300 directory hides its files from `ls`, so
"cannot tell" must never mean "empty"), and at the scaffold re-check just before seeding any entry
this run did not create is refused (`refusing to start at …: it is not a rig root and holds
something this run did not create (found '<entry>')`, or, when the root already was a rig root,
`…: something this run did not create appeared in it (found '<entry>')`). A stopped rig root is
re-started with its whole inventory intact (logs, `pids`, `display`, `rig.env`), so the re-check
tolerates those and only rejects a genuinely foreign entry. The root is marked (`.rig-root`) as soon as
the takeover check passes, so an aborted start — a missing `--track`, the version-probe timeout —
leaves a root that `start`, `restart` and `stop --purge` still recognise. The rig root is exported as `LX_RIG_ROOT` to
every child, and `$RIG` below means it.
`--track FILE` uses FILE exactly as it is (a missing file is an error; a missing `.lrc` sidecar
next to it only warns, since the file may be real music) — the silent fixture is generated only
at the default `<rig-root>/fixtures/track.flac`.

Exit status: `0` success (including a `stop` that found nothing to do), `1` a refusal or an
error, `3` a teardown verification failure — a rig-owned process survived, or the rig wrote a
file outside its root (see `stop`/`status` below). `rig.sh --help` prints the same table.

`appwrap.py` is Fooyin's `[LxLyrics] AppPath` (`LxLyrics/AppPath`,
`plugins/fooyin/src/lxlyricssettings.h:30`). `seed-config.py` copies it into the rig root
(`$RIG/appwrap.py`, mode 755 — the plugin execs its AppPath, so the file needs the exec bit and
the checkout must never be written to) and writes *that* path into the rig's `fooyin.conf`; the
plugin spawns it as its "app" (with `--player-feed`; unknown arguments are forwarded to the
real app). It **runs** the app as a child — `subprocess.Popen`, one pump thread per direction,
a reader deadline, `SIGTERM` then `SIGKILL` for a child that ignores stdin EOF — and exits with
the child's own code; it is not an `exec`, because the plugin's contract is a direct child. Both
pipes are tee'd, one line per wire line into `$RIG/feed.log`, prefixed `<< ` (host→app) or
`>> ` (app→host). The app's stderr needs no tee: it is inherited, and the plugin already drains
it into `fooyin.log` prefixed `[LX Lyrics app]` (`plugins/fooyin/src/feedwriter.cpp:247`).

## Prerequisites

- `fooyin` on `$PATH` — **0.12 / 0.12.x**, tested with 0.12.6. `start` asserts it: `fooyin
  --version` (bounded by `timeout 10`, so a wedged player cannot hang the start) must print a
  `0.12` or `0.12.x` line, otherwise it dies with
  `error: unsupported fooyin version: 'fooyin 0.11.0' (this rig encodes 0.12.x config keys; pass
  --force to start anyway)`. The rig encodes version-specific config keys (`[LxLyrics]`,
  `[Engine] AudioOutput`, the shortcut), so an unknown major/minor is refused rather than run
  with keys that may no longer exist; `--force` is the documented escape hatch and is listed in
  `--help`. A probe that does not answer within 10 s dies with `error: 'fooyin --version' did not
  answer within 10s (a wedged fooyin, or a stale single-instance socket?)`.
- Qt **6.11.2** (the version this rig was tested against; Fooyin is a Qt app, and the rig does
  not pin Qt itself).
- `Xvfb`, `xdotool` and ImageMagick `import`; `xwininfo` (used by the `xwininfo -root -tree`
  hint `start` prints). `start` dies immediately when `Xvfb` is missing (`Xvfb is not on PATH
  (the rig needs its own X display)`) instead of burning the 45 s plugin timeout on a display
  that never comes up, and warns when `xdotool`/`xwininfo` are missing (they are only needed
  for `key`, the window listing and those hints).
- `dbus-daemon` — the rig runs a private session bus.
- `python3` — `seed-config.py`, `appwrap.py`.
- `ffmpeg` — generates the fixture FLAC (`ffprobe`, optional, sanity-checks it).
- The **app** binary, in this order: `--app PATH`, the repo build
  `lyrics-app/build/lx-lyrics-app`, `$HOME/.local/bin/lx-lyrics-app`, then the path a previous
  seed recorded in `$RIG/app-path`. `start` dies when none of them is executable. Build it with
  `cd lyrics-app && cmake -B build -G Ninja && cmake --build build`.
- The **plugin** does **not** have to be installed system- or user-wide: `seed-config.py` copies
  `plugins/fooyin/build/fyplugin_lxlyrics.so` into
  `$RIG/home/.local/lib/fooyin/plugins/`, and builds it first
  (`cmake -B build -G Ninja && cmake --build build` in `plugins/fooyin/`) when the artifact is
  missing — so `cmake` + `ninja` are needed only for that first build. That build needs Fooyin's
  CMake package (`find_package(Fooyin REQUIRED)` in `plugins/fooyin/CMakeLists.txt`): either a
  Fooyin built/installed with `INSTALL_HEADERS=ON`, or the distro's development package. The
  copy is refreshed only when the two files differ.

## Recipe

```sh
# Pick the root once and use it everywhere. `--rig-root "$RIG"` on every rig.sh line is what
# makes the low-level commands below address the root you actually started; a bare
# `export LX_RIG_ROOT="$RIG"` would do the same for the lines that omit the flag, but only for
# shells that keep the export (and only for the *default* root if you never export it).
RIG=/tmp/lx-lyrics-gui-rig

# 1. start: seed the rig, allocate a display, start Xvfb + a private dbus-daemon + fooyin,
#    wait for the LX Lyrics GuiPlugin to initialise, then print the follow-up commands.
tools/gui-test/rig.sh --rig-root "$RIG" start

# 2. drive the rig through its own subcommands. They carry the rig's environment (HOME, the
#    XDG roots, DISPLAY and TMPDIR) themselves — and TMPDIR is what locates fooyin's
#    single-instance socket, so only these (or a shell that sourced rig.env) reach the rig's
#    instance instead of starting a second fooyin. The seeded desktop-lyrics state is OFF, so
#    this first ctrl+i is what spawns the app and opens feed.log.
tools/gui-test/rig.sh --rig-root "$RIG" key --clearmodifiers ctrl+i   # the Desktop Lyrics toggle
sleep 2
tools/gui-test/rig.sh --rig-root "$RIG" fooyin -p "$RIG/fixtures/track.flac"
sleep 2                                    # the null sink is unthrottled: pause before reading
tools/gui-test/rig.sh --rig-root "$RIG" fooyin -u
tools/gui-test/rig.sh --rig-root "$RIG" fooyin -R 600000
tools/gui-test/rig.sh --rig-root "$RIG" fooyin -F 150000

# screenshots need the rig's DISPLAY. Use the `app win 0x…` id `status` prints (it filters the
# 1x1 Qt helper windows); the app window only exists after the toggle. `xdotool search
# --classname lx-lyrics-app | head -n 1` can pick a helper, so do not use it blindly.
tools/gui-test/rig.sh --rig-root "$RIG" status            # -> app win 0x400007 Geometry: 300x300
DISPLAY=":$(cat "$RIG/display")" import -window 0x400007 shot.png

# frame burst (the plan's observation recipe) — 24 root grabs, one every ~0.1 s:
mkdir -p burst && cd burst
for i in $(seq -w 1 24); do DISPLAY=":$(cat "$RIG/display")" import -window root "q_$i.png"; sleep 0.1; done

# the player-quit path is one of the lifecycle bugs the rig exists for:
tools/gui-test/rig.sh --rig-root "$RIG" key --clearmodifiers ctrl+q
# then look for: feed.log stops growing, the plugin's child exits, and fooyin.log ends with
#   [LX Lyrics] shutdown
#   [LX Lyrics app] feed: host closed the pipe, exiting

# lower-level alternative: source rig.env, then use the raw tools yourself. rig.env is written
# with `export NAME=value` lines, so sourcing it is enough — it hands TMPDIR to the fooyin CLI
# and DISPLAY to xdotool/import. (But see "Failure modes": anything you launch from a *sourced*
# shell carries rig paths in its environment and becomes a `stop` sweep candidate — including a
# `tail -f`. Run tails from an unsourced shell if you want them to survive.)
source "$RIG/rig.env"
tail -f "$RIG/fooyin.log"     # fooyin + plugin logs; app stderr lines are tagged [LX Lyrics app]
tail -f "$RIG/feed.log"       # the wire: '<< ' host→app, '>> ' app→host (empty until the toggle)
WIN=$(xdotool search --classname fooyin | head -n 1)
xdotool windowfocus "$WIN" && xdotool key --clearmodifiers ctrl+i
fooyin -p "$RIG/fixtures/track.flac"; sleep 2; fooyin -u; fooyin -R 600000; fooyin -F 150000

# 3. stop — validates every pid first, keeps the rig root and its logs, exits 0 when
#    nothing was running (3 when something survived or a user file changed).
tools/gui-test/rig.sh --rig-root "$RIG" stop
tools/gui-test/rig.sh --rig-root "$RIG" stop --purge     # …and remove the rig root
```

What the commands do beyond the above:

- `fooyin <args…>` runs the fooyin CLI with the rig's environment (HOME, the XDG roots, DISPLAY,
  TMPDIR, and `WAYLAND_DISPLAY`/`XDG_SESSION_TYPE` unset) and is the supported way to drive
  `-p/-u/-t/-s/-R/-F`. It refuses when the rig is not running (`error: 'rig.sh fooyin' needs a
  live rig: …`), and with no arguments it **refuses** too: `error: usage: rig.sh fooyin <fooyin
  args...>  (e.g. rig.sh fooyin -p <file>)` on stderr, exit 1.
- `key <args…>` focuses the fooyin main window on the rig's display and runs `xdotool key` with
  the remaining arguments (`rig.sh key --clearmodifiers ctrl+i`); it refuses when the rig is not
  running, or when no main window is on the display (`no fooyin main window on :N`). The window
  is the first `fooyin`-class window wider than 1 px (Qt keeps 1x1 helpers around), the same
  helper `status` uses.
- `start` prints the rig root, the display, the pids, the app and fixture paths, the log paths,
  the `tmp/` dir, the follow-up commands and the whole CLI table. Every `rig.sh` line in that
  follow-up block spells out `--rig-root <rig root>`, so it is the authoritative copy for the
  root you actually started. It copies `appwrap.py` into the rig root, asserts the Fooyin
  version, baselines the operator's own fooyin/lx-lyrics file mtimes in `$RIG/user-files.mtimes`
  *before* seeding (so a leak during seeding cannot become the baseline), writes `$RIG/.rig-root`
  (the marker that licenses the sweep and `--purge`), then starts Xvfb (via
  `-displayfd`, so the number is allocated and never hardcoded), a private `dbus-daemon` and
  fooyin. If the rig already looks alive it warns, prints `status` and exits 0 instead of
  rebuilding. If `GuiPlugin initialised` does not appear in `fooyin.log` within
  `LX_RIG_START_TIMEOUT` (45 s default), it tails the log, tears the rig down and exits 1.
- `restart` is literally `stop` then `start`: the rig root and the four log files survive, but
  `start` truncates them, re-seeds `fooyin.conf` and re-asserts the plugin — nothing of the
  previous run's logs is left. A `stop` that fails its verification (exit 3) aborts the restart.
- `status` prints each recorded pid still matching its identity, the display, the log paths,
  whether `GuiPlugin initialised` was seen, the single-instance socket found under `tmp/`, the
  main and app window ids with geometry, and the CLI table; exits 0 when stopped. It re-checks
  `user-files.mtimes` unconditionally — a crashed rig can still have leaked — and exits **3** when
  one of the operator's files changed while the rig ran, even when the pid table reads
  `not running`.
  Two warnings appear in the *running* output only (the stopped path returns early with
  `not running`): a dead recorded fooyin — `the recorded fooyin is gone (stale rig root); run
  'rig.sh restart'` — and another fooyin on the machine (`another fooyin is running: … (its
  socket is elsewhere; not touched)`).
- `stop` signals a recorded pid **only** while `/proc/<pid>` still proves it is that process:
  `/proc/<pid>/exe` resolves to the recorded binary **and** the recorded start time (field 22 of
  `/proc/<pid>/stat`) still matches. A basename match is deliberately not enough — a stale
  `pids` line whose number the operator's own `fooyin` has since been given must not be
  terminated. A record that cannot be verified (a legacy 3-field line with no start time, an
  unreadable `/proc/<pid>/exe`) is **skipped, not signalled**, and named:
  `warning: skipping recorded fooyin pid 1234: cannot prove it is /usr/bin/fooyin (no recorded
  start time, or /proc/1234/exe unreadable)`. On top of the records, and **only when the rig root
  carries its `.rig-root` marker**, the rig sweeps `/proc/*/environ` once and signals pids whose
  *launch* environment contains a value equal to or under the rig root: `LX_RIG_ROOT=$RIG`, or
  `HOME=$RIG/home`, `TMPDIR=$RIG/tmp`, `XDG_*=…`. That is **proof of ownership, not proof the rig
  started the process** — it is how the plugin's child app, an `appwrap` and the services the
  rig's own private bus activated (portals, at-spi, gvfsd, ksecretd) are found; the matched
  `KEY=VALUE` is printed per pid as the evidence. Three things are never signalled: this shell and
  its ancestors, shell interpreters (a terminal *launched* with rig paths — `source rig.env` in
  place leaves nothing in `/proc/<pid>/environ` — is spared by argv[0] *and* by its resolved
  executable, so the login spelling `-zsh` is covered), and anything whose path lives under a
  *different*, nested rig root (`belongs to the nested rig root …/child, not …`). TERM first,
  then KILL for survivors after ~5 s; eligibility is re-proved immediately before each KILL, so a
  pid that exited or was recycled in between is skipped and named instead. Rig-owned pids that
  survive teardown are reported with their cmdline and make `stop` exit **3**. A directory that
  never was a rig root is a silent no-op (`not running (no rig root at …)`, exit 0).
- `stop --purge` deletes the rig root only when teardown left nothing behind — otherwise it
  reports the survivors and keeps the root and the logs (exit 3). A user-file change does **not**
  block it: the tripwire is a warning, so the run exits 3 and the purge still happens when it was
  asked for (its warnings are already on screen). The
  deletion itself refuses any directory that is not a rig root (no `.rig-root` marker and no
  `rig.env` naming it — a root written by the previous revision is still recognised through its
  `rig.env`), one that contains a nested rig root, and (through the shared validator) `/`, the
  empty path, top-level directories, `$HOME`, the repository, and any directory containing
  either. A refusal says so and prints the exact command for a human to run if they really mean
  it: `rm -rf -- '<dir>'`.
- `start` and `status` print the CLI table (the fooyin flags, the millisecond seek rule, the
  pause-step recipe) after their own output; `--help` prints the synopsis with every subcommand
  and option, the exit statuses, and every environment knob the script honours
  (`LX_RIG_ROOT`, `LX_RIG_DISPLAY_SIZE`, `LX_RIG_START_TIMEOUT`), including the "options come
  before the subcommand" rule. When this file and the scripts disagree, the printed help is the
  source of truth.

## Inside the rig root

| Path | What it is |
|---|---|
| `.rig-root` | the marker that proves this directory is a rig root: the environment sweep and `--purge` run only for a marked root (a previous revision's root is recognised through its `rig.env`) |
| `rig.env` | sourceable (`export NAME=value` lines): `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`, `XDG_RUNTIME_DIR`, `TMPDIR`, `DISPLAY` (`':2'`), `DBUS_SESSION_BUS_ADDRESS`, `LX_RIG_ROOT`, `LX_RIG_APP`, `LX_RIG_TRACK`, `FOOYIN_VERSION` |
| `pids` | `<name> <pid> <binary> <starttime>` per line — `fooyin`, `dbus`, `xvfb`; the only processes `stop` may signal, and only while `/proc/<pid>/exe` and the recorded start time still match |
| `user-files.mtimes` | the operator's own `~/.config/lx-lyrics/config.json`, `~/.config/fooyin/fooyin.conf` and `~/.local/lib/fooyin/plugins/fyplugin_lxlyrics.so` with the mtime `start` saw (or `absent`); `stop`/`status` re-check them and name any that moved |
| `appwrap.py` | the mode-755 AppPath wrapper (a copy of `tools/gui-test/appwrap.py`); `[LxLyrics] AppPath` points at this copy, never at the checkout |
| `display` | the bare Xvfb display number (`2`, i.e. `:2`) |
| `app-path` | the resolved app binary (also `start`'s last-resort `--app` fallback, and `appwrap.py`'s) |
| `fooyin.log` | fooyin's own log, the plugin's lines, and the app's stderr as `[LX Lyrics app] …` |
| `feed.log` | the v2 wire — `<< ` host→app, `>> ` app→host, one line per JSON message (`docs/protocol.md` §2); empty until the app is spawned |
| `xvfb.log`, `dbus.log` | Xvfb's and the private bus's output |
| `home/` | the private `HOME` + XDG tree (config, share, state, cache) |
| `run/` | private `XDG_RUNTIME_DIR` (mode 700) |
| `tmp/` | private `TMPDIR` (mode 700) — the rig's single-instance socket (`kdsingleapp-<user>-fooyin`) is created here; `status` prints the path |
| `fixtures/track.flac` + `track.lrc` | the 300 s silent fixture and its sidecar lyrics |

`start` truncates the four logs, so a fresh run starts clean. `feed.log` stays **empty until the
app is spawned** — the wire opens with the `hello` handshake at spawn, which the `Ctrl+I` toggle
(or any other start of desktop lyrics) triggers, and the seeded `[LxLyrics] Enabled=false` means
the first toggle is what spawns it — so an empty `feed.log` means "no app yet", not a broken rig.

## Environment facts the rig encodes

`file:line` citations are from the pinned `references/fooyin-0.12.6/` (v0.12.6) checkout — a
bare `src/…` path is relative to it.

- **Isolation** = a private `HOME`, private `XDG_*` roots, a private `TMPDIR`, and
  `QT_QPA_PLATFORM=xcb` with `WAYLAND_DISPLAY`/`WAYLAND_SOCKET`/`XDG_SESSION_TYPE` dropped for
  the child. Each of the three roots covers a different Fooyin path, so none is optional.
- **`HOME` is mandatory**, because Fooyin's per-user plugin dir is HOME-derived, not
  `XDG_DATA_HOME`-derived (`src/core/corepaths.cpp:61`):

  ```cpp
  QString userPluginsPath()
  {
      return Utils::createPath(QDir::cleanPath(u"%1/.local/lib/fooyin/plugins/"_s.arg(QDir::homePath())));
  }
  ```

  So the rig's plugin dir is `$HOME/.local/lib/fooyin/plugins` *inside the rig root* (which is
  what `seed-config.py` populates) — and the reason an unset `HOME` writes into the user's own
  plugin dir.
- **`TMPDIR` is mandatory** for the same reason on the CLI side: a `fooyin` invocation is not a
  second instance, it hands its options to the primary instance through KDSingleApplication
  (`KDAB::kdsingleapplication`, `CMakeLists.txt:446`; forwarding at `src/app/main.cpp:161-176`),
  whose socket is named `kdsingleapp-<user>-fooyin` and resolved through `QDir::tempPath()` —
  i.e. `$TMPDIR`. `XDG_RUNTIME_DIR` does **not** cover it, and the socket is per-user and
  machine-wide by default, so the rig points `TMPDIR` at `$RIG/tmp`. That is what pins
  `fooyin -p/-u/-R/-F` to the rig's instance, and why `start` warns (and starts anyway) when
  another fooyin is already up: with its own `TMPDIR` the rig cannot collide with that player,
  and a fooyin launched by hand with the ambient `TMPDIR` cannot steal the rig's commands. Run
  the CLI as `rig.sh fooyin …` (or after `source $RIG/rig.env`): a raw `fooyin` in an unsourced
  shell resolves the socket in the ambient temp dir and starts a *second* fooyin instead. The
  version probe `start` runs for the assert uses the same `TMPDIR`, so even it cannot touch the
  ambient socket.
- **Config** lives under the XDG config dir (`corepaths.cpp:35` →
  `Utils::configPath().append("/fooyin.conf")`, and `Utils::configPath()` is
  `QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)`,
  `src/utils/fypaths.cpp:79`) — i.e. `$XDG_CONFIG_HOME/fooyin/fooyin.conf`, which
  `seed-config.py` writes. Seeding it also suppresses the "Quick Setup" wizard: `FirstRun` is
  literally "the config file does not exist yet" (`src/core/internalcoresettings.cpp:164`), and
  the main window shows the dialog only when it is set (`src/gui/guiapplication.cpp:643`). The
  seeded content is `[General] LogLevel=4` — the plugin's own `GuiPlugin initialised` line is a
  `qInfo`, and `start`'s assertion greps for it, so the level must be low enough to let it into
  `fooyin.log` — plus `[Engine] AudioOutput=ALSA|null`,
  `[KeyboardShortcuts] LxLyrics.DesktopLyrics=Ctrl+I`, and `[LxLyrics]` `AppPath=$RIG/appwrap.py`,
  `Enabled=false` (desktop lyrics start off), `RememberState=false`.
- **Library DB** `$XDG_DATA_HOME/fooyin/fooyin.db` (`src/utils/fypaths.cpp:118` +
  `src/core/database/database.cpp:41`) — `$HOME/.local/share/fooyin/fooyin.db` when
  `XDG_DATA_HOME` is unset. **State** `$XDG_STATE_HOME/fooyin/fooyin.state` (`corepaths.cpp:38`,
  `fypaths.cpp:95-99`) — `$HOME/.local/state/fooyin/fooyin.state` by default. The app's own
  config is a third root: `$XDG_CONFIG_HOME/lx-lyrics/config.json`
  (`lyrics-app/src/config/desktoplyricconfig.cpp:303-306`).
- A **private `dbus-daemon`** contains portal/ksecretd activation: nothing on the user's bus
  is talked to, and nothing prompts. (Those activated services inherit the rig's environment,
  which is exactly why `stop`'s sweep finds them.)
- **No window manager is needed** — the main window maps on bare Xvfb. Keys go through
  `xdotool windowfocus <id>` + `xdotool key --clearmodifiers ctrl+i`; the windows are found by
  class name (`fooyin`, `lx-lyrics-app` — the Fooyin main window is also titled `fooyin`,
  `src/gui/mainwindow.cpp:178`).
- **The display is allocated**, never hardcoded: Xvfb is started with `-displayfd` and picks a
  free number, read back from `$RIG/display` (bare number) or `$RIG/rig.env` (`:N`). Screen
  size is `LX_RIG_DISPLAY_SIZE` (default `1440x900x24`). Another rig (or a stale one) may
  already own a display — a stray ad-hoc rig was once observed on `:99`.
- **Audio**: `[Engine] AudioOutput=ALSA|null` (`Engine/AudioOutput`,
  `src/core/internalcoresettings.cpp:80`) initialises and plays with no sound hardware; the
  acceptance run logged `fy.engine: Output initialised: "44100Hz 1ch 16 bit (signed) [FC]"
  (device: "null" )`. But the null sink is **unthrottled** — a 300 s track runs far ahead of
  realtime — so deterministic frames come from **pause-stepping** the running instance:

  ```sh
  tools/gui-test/rig.sh fooyin -p "$RIG/fixtures/track.flac"   # -p/--play; positional URL = file to open
  tools/gui-test/rig.sh fooyin -u                              # -u/--pause
  tools/gui-test/rig.sh fooyin -R 600000                       # -R/--seek-backward <ms>: past 0, clamps to 0
  tools/gui-test/rig.sh fooyin -F 150000                       # -F/--seek-forward <ms>: lands on 150000
  ```

  (Bare `fooyin -p …` works the same once the shell has sourced `rig.env`.) `-s` stops, `-t`
  toggles play/pause (option table `src/app/commandline.cpp:62-110`). The
  value is **milliseconds**, and only the leading digits are read (`std::stoull`,
  `commandline.cpp:160-183`), so `-R 150s` seeks 150 ms, not 150 s — measured in the
  acceptance run: `-R 150s` moved `played_time` 201810 → 201660, and the recipe above lands on
  exactly 150000 ms.
- **Setup assertion**: `rig.sh start` waits for `GuiPlugin initialised` in `$RIG/fooyin.log`
  (that line is the plugin's own, `plugins/fooyin/src/lxlyricsplugin.cpp:134`), so a green
  start proves the adapter was actually loaded by the running app.

## A real run (acceptance evidence)

Both excerpts are **post-toggle**: `feed.log` is empty until the `Ctrl+I` toggle spawns the app,
and the wire's first line is then the `hello` handshake shown here.

```console
$ head -3 "$RIG/feed.log"
<< {"action":"hello","host":"fooyin","spectrum":true,"v":2}
<< {"action":"set_fullscreen","isFullscreen":false,"v":2}
<< {"action":"set_info","album":"","isPlay":false,"name":"","path":"","played_time":0,"singer":"","v":2}

$ tail -8 "$RIG/feed.log"    # after: rig.sh fooyin -p <track>; sleep 2; rig.sh fooyin -u; rig.sh fooyin -R 600000; rig.sh fooyin -F 150000
<< {"action":"set_status","isPlay":true,"played_time":207321,"v":2}
<< {"action":"set_status","isPlay":true,"played_time":211114,"v":2}
<< {"action":"set_status","isPlay":false,"played_time":211142,"v":2}
<< {"action":"set_status","isPlay":false,"played_time":214911,"v":2}
<< {"action":"set_play","time":0,"v":2}
<< {"action":"set_status","isPlay":false,"played_time":0,"v":2}
<< {"action":"set_play","time":150000,"v":2}
<< {"action":"set_status","isPlay":false,"played_time":150000,"v":2}
```

Representative `fooyin.log` lines from the same run — plugin loaded, the wrapper spawned from
the rig root, the app mapped on the rig's `xcb` display, and the fixture track parsed with its
181 lyric characters. Note `config x/y` prints `QVariant(Invalid)` for **both** coordinates:

```console
[LX Lyrics] GuiPlugin initialised; actionManager stored; 'Desktop Lyrics' toggle added to View menu
[LX Lyrics] spawned lyrics-app: "/tmp/lx-lyrics-gui-rig/appwrap.py" "--player-feed"
[LX Lyrics] lyrics app started
[LX Lyrics app] [LX Lyrics] platform: "xcb" geometry: QRect(570,300 300x300) config x/y: QVariant(Invalid) QVariant(Invalid)
[LX Lyrics app] feed: connected to "fooyin" spectrum: true
[LX Lyrics app] feed: track "<unknown>" "" "" playing: false lyric chars: 0
[LX Lyrics] track changed: "LX Lyrics Rig" - "Rig Fixture Track"
[LX Lyrics app] feed: track "Rig Fixture Track" "- LX Lyrics Rig" "[/tmp/lx-lyrics-gui-rig/fixtures/track.flac]" playing: true lyric chars: 181
```

And the quit path — `rig.sh key --clearmodifiers ctrl+q` — ends the session the way the
lifecycle bugs are read:

```console
[LX Lyrics] shutdown
[LX Lyrics app] feed: host closed the pipe, exiting
```

`status` in that run, on a machine where the operator's own Fooyin was also up:

```console
$ tools/gui-test/rig.sh status
fooyin  pid 476392  /usr/bin/fooyin
dbus  pid 476385  /usr/bin/dbus-daemon
xvfb  pid 476369  /usr/bin/Xvfb

rig running
  rig root  /tmp/lx-lyrics-gui-rig
  display   :2
  plugin    GuiPlugin initialised
  warning   another fooyin is running: 455963 (its socket is elsewhere; not touched)
  socket    /tmp/lx-lyrics-gui-rig/tmp/kdsingleapp-naromil-fooyin
  main win  0x200008 Geometry: 864x540
  app win   0x400007 Geometry: 300x300
```

`stop --purge` then killed the 3 recorded pids (`fooyin`, `dbus`, `Xvfb`) plus the 15 rig-owned
processes the sweep found — the plugin's `appwrap` + app and the services the rig's private bus
had activated, all carrying `LX_RIG_ROOT=/tmp/lx-lyrics-gui-rig` or `HOME=…/home` — purged the
root, and left the operator's Fooyin (`455963`) and its `lx-lyrics-app` child (`455990`) running:
they carry no rig path, so they were never sweep candidates.

## Failure modes

- **Inherited `WAYLAND_DISPLAY`/`XDG_SESSION_TYPE`** — the child keeps talking to the user's
  Wayland session, the window never maps on the rig's X display, and the "rig" is a copy of
  the user's player. Start from `rig.env`, never from the ambient environment.
- **A `fooyin` invocation without the rig's environment** — the CLI locates the single-instance
  socket through `TMPDIR`, so a raw `fooyin -p …` in a shell that never sourced `rig.env`
  resolves it in the ambient temp dir: instead of driving the rig's instance it starts a
  **second fooyin**, which swallows the command and then hangs (the line never returns) and
  leaves `/tmp/kdsingleapp-<user>-fooyin` behind. That is the same mechanism by which the user's
  own player gets swallowed. Drive the CLI with `rig.sh fooyin …`/`rig.sh key …`, or
  `source "$RIG/rig.env"` first.
- **Forgotten `HOME`** — a private `XDG_*` alone is not isolation: the per-user plugin dir,
  the share DB and the state file revert to the user's real home, and the rig starts writing
  into the user's Fooyin state. `HOME` is the one variable that must never be inherited.
  `user-files.mtimes` is the tripwire for exactly this: `stop`/`status` re-check the operator's
  `~/.config/fooyin/fooyin.conf`, `~/.config/lx-lyrics/config.json` and
  `~/.local/lib/fooyin/plugins/fyplugin_lxlyrics.so` and exit 3 naming the file whose mtime
  moved.
- **The tripwire names a change, not its author** — the message is deliberately neutral:
  `user-session file changed while the rig ran: <file> (mtime <old> -> <new>)` followed by
  `by the rig, or by another writer in your session (your own Fooyin/app saves its config)`.
  Your own Fooyin or app writing its config while a rig is up produces exactly the same warning as
  a rig leak, so do not read it as proof: compare the two mtimes against the run window, or run
  the rig with nothing else touching those files. Only survivors block a `--purge`; a tripwire hit
  warns, makes the run exit 3, and (when you asked for it) still removes the root.
- **No dedicated display** — running against `:0` (or an assumed `:99`) drives the user's
  desktop and stomps another rig's session. Read the display from `rig.env`.
- **Assuming realtime playback** — with `ALSA|null` the track is over before the first frame
  renders. Pause-step (`-u`, then `-R <ms>`, `-F <ms>`) instead of racing the transport.
- **Plugin not visible to the rig** — a private HOME hides the user's
  `~/.local/lib/fooyin/plugins`; `seed-config.py` is what puts the adapter in the rig's own
  plugin dir, so a failed build/copy shows up as `start` dying on the `GuiPlugin initialised`
  assertion with an otherwise clean log.
- **A different Fooyin** — the rig's config keys are 0.12.x-shaped. `start` refuses another
  version before it seeds or starts anything; `--force` overrides that deliberately.
- **Signalling by name** — the rig signals only what it can prove: a recorded pid whose
  `/proc/<pid>/exe` and start time still match, plus pids whose *launch* environment carries a
  path equal to or under the rig root (ownership proof — not proof the rig started them). Never
  `pkill` by name, and never touch a rig that is not yours. A pid it cannot verify is skipped
  and named, never signalled.
- **`tail -f` from a sourced shell** — the tail inherits `HOME=$RIG/home`, `TMPDIR=$RIG/tmp`
  and the rest, so it looks rig-owned to the sweep and `stop` reaps it (and any other tool you
  started from that shell). Run tails from an unsourced shell, or expect them to go.
- **Signalling an operator shell** — `/proc/<pid>/environ` exposes the environment a process
  was *started* with, so a shell that ran `source rig.env` **in place** is not a sweep candidate
  at all. The guard covers a shell **launched** with rig paths in its environment — e.g.
  `env HOME=$RIG/home TMPDIR=$RIG/tmp bash` — which would otherwise look rig-owned: `stop`
  skips shell interpreters by argv[0] (including the login spelling `-zsh`) and by the resolved
  executable (`a shell (zsh) that sourced rig.env, not a rig process`), plus its own
  shell/ancestors. Keep that guard if you script teardown yourself.
- **Nested rig roots** — a root inside another root: the parent's sweep would otherwise own the
  child's processes and `--purge` would delete the child. `start` and `--purge` refuse a root
  that contains another rig root, and the sweep skips any process whose path lives under a
  different, nested root, naming it: `not signalling pid 1234: HOME=…/child/home belongs to the
  nested rig root …/child, not …`. Pre-marker-era nested roots carry no `.rig-root`, so the
  walk-up cannot see them (the containment scan itself is not depth-limited, so a nested root is
  found however deep it sits).

## Limits (what the rig cannot prove)

- The `.rig-root` marker and a legacy `rig.env` are **operator declarations**, not proof of
  rig-ness: anyone who names that directory to `rig.sh` can license its sweep and its `--purge`.
  They exist to stop accidents, not an attacker.
- A rig root **inside `$HOME`** is allowed (`--rig-root ~/newdir`): the non-empty takeover refusal
  bounds what a `--purge` can destroy to a directory that was empty (or did not exist) when
  `start` took it over, and refusing `$HOME` subtrees outright would ban legitimate locations.
- The takeover check is **not atomic**: it runs before the scaffold and again immediately before
  seeding, but a writer that fills the directory in the window between those two checks is taken
  over (an atomic `mkdir`-based redesign is not worth it here).
- `realpath` cannot see through a bind mount, so a bind-mounted rig root is out of scope: the
  shared validator compares text, not inodes.
- Two processes started within the same clock jiffy share a start-time value, so identity needs
  the executable path as well — which is why `pid_is` requires both `/proc/<pid>/exe` *and* field
  22 of `/proc/<pid>/stat`.
- A pre-marker-era *nested* rig root carries neither `.rig-root` nor (necessarily) a matching
  `rig.env`, so the walk-up cannot see it: `start`/`--purge` would not refuse it and the sweep
  would not skip it.

## Conventions

Repo-wide: `SPDX-License-Identifier` header at the top of every source file. These scripts
are **outside** `tools/lint.sh` — that gate is clang-format/clang-tidy over C++ only — so
they are hand-formatted, bash + python3 only, no new dependencies.
