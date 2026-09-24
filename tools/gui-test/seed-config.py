#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 LX Lyrics contributors.
#
# Seed the private configuration and fixtures of the Fooyin GUI test rig.
#
#   tools/gui-test/seed-config.py [--rig-root DIR] [--app PATH] [--track PATH]
#
# Everything this writes lives under <rig-root> (default /tmp/lx-lyrics-gui-rig)
# and is never written outside it except for the one case named below:
#
#   <rig-root>/home/.config/fooyin/fooyin.conf   Fooyin settings, incl.
#       [KeyboardShortcuts] LxLyrics.DesktopLyrics=Ctrl+I, [Engine]
#       AudioOutput=ALSA|null (unthrottled, no hardware), and [LxLyrics]
#       AppPath=<rig-root>/appwrap.py (the feed tee the plugin execs - a copy
#       this script puts in the rig root, so the checkout is never written to),
#       Enabled=false, RememberState=false. Seeding this file at all is what
#       clears Fooyin's first-run "Quick Setup" dialog: Fooyin derives
#       FirstRun from `!QFileInfo::exists(Core::settingsPath())`
#       (core/internalcoresettings.cpp), so an existing fooyin.conf skips it.
#   <rig-root>/home/.local/lib/fooyin/plugins/fyplugin_lxlyrics.so
#       A copy of plugins/fooyin/build/fyplugin_lxlyrics.so. Fooyin scans
#       $HOME/.local/lib/fooyin/plugins for user plugins (HOME-derived, NOT
#       $XDG_DATA_HOME), so a private HOME needs its own copy. The artifact is
#       built here (cmake -B build -G Ninja && cmake --build build) when it is
#       missing; the copy is refreshed only when the two files differ, so
#       re-running is a no-op.
#   <rig-root>/fixtures/track.flac (+ track.lrc sidecar)
#       A 300 s silent tagged FLAC plus the lyrics the app reads back, so the
#       feed has something to parse. Generated only when missing.
#   <rig-root>/appwrap.py
#       A mode-755 copy of tools/gui-test/appwrap.py: the [LxLyrics] AppPath the
#       plugin execs. The repository copy is never chmod-ed, so a fresh checkout
#       without the exec bit still works.
#   <rig-root>/app-path
#       The resolved lyrics-app binary, recorded for rig.sh's last-resort
#       --app fallback, for appwrap.py, and for humans reading the rig root.
#
# --track names a fixture of your own: an existing file is used exactly as it
# is (never rewritten, and no sidecar is dropped next to it — that file may be
# real music), a missing one is an error. Omit --track to get the generated
# default fixture.
#
# The script is idempotent: it rewrites a file only when its content differs,
# so unchanged files keep their mtime.
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
APPWRAP = REPO_ROOT / "tools" / "gui-test" / "appwrap.py"
PLUGIN_PROJECT = REPO_ROOT / "plugins" / "fooyin"
PLUGIN_ARTIFACT = PLUGIN_PROJECT / "build" / "fyplugin_lxlyrics.so"
DEFAULT_RIG_ROOT = os.environ.get("LX_RIG_ROOT") or "/tmp/lx-lyrics-gui-rig"
TRACK_SECONDS = 300

# Sidecar lyrics: lines at 0 s, 5 s, 60 s and 150 s, matching the fixed
# pause-step positions the rig's acceptance run drives the player to.
LRC_TEXT = """[00:00.00]Empty streets at midnight
[00:05.00]The rig fixture is playing
[01:00.00]A line that only renders in the second minute
[02:30.00]And one that is only reached after a seek
"""


def note(message: str) -> None:
    print(f"    {message}")


def step(message: str) -> None:
    print(f"\n==> {message}")


def warn(message: str) -> None:
    print(f"warning: {message}", file=sys.stderr)


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def abspath(path: str) -> Path:
    return Path(os.path.abspath(os.path.expanduser(path)))


def resolve_app(given: str | None) -> str | None:
    """The lyrics-app binary from --app or the ambient candidates: an explicit
    path, then the repo build, then the user install. The path an earlier seed
    recorded in <rig-root>/app-path is handled by main(), which knows the rig
    root."""
    candidates = []
    if given:
        candidates.append(given)
    else:
        candidates.append(str(REPO_ROOT / "lyrics-app" / "build" / "lx-lyrics-app"))
        candidates.append(os.path.join(os.path.expanduser("~"), ".local", "bin", "lx-lyrics-app"))
    for candidate in candidates:
        if os.access(candidate, os.X_OK):
            return candidate
    return None


def write_if_changed(path: Path, content: str) -> bool:
    """Write content only when the file differs; returns True when written."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8", errors="replace") == content:
        return False
    path.write_text(content, encoding="utf-8")
    return True


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_plugin(rig: Path) -> Path:
    dest = rig / "home" / ".local" / "lib" / "fooyin" / "plugins" / PLUGIN_ARTIFACT.name

    if not PLUGIN_ARTIFACT.exists():
        # N10: a missing cmake/ninja must produce this script's own error line,
        # not a FileNotFoundError traceback from inside subprocess.
        for tool in ("cmake", "ninja"):
            if not shutil.which(tool):
                die(f"{tool} is required to build the Fooyin plugin ({PLUGIN_PROJECT})")
        step(f"building the Fooyin plugin ({PLUGIN_PROJECT})")
        subprocess.run(["cmake", "-B", "build", "-G", "Ninja"], cwd=PLUGIN_PROJECT, check=True)
        subprocess.run(["cmake", "--build", "build"], cwd=PLUGIN_PROJECT, check=True)
    if not PLUGIN_ARTIFACT.exists():
        die(f"the Fooyin plugin artifact is still missing after a build: {PLUGIN_ARTIFACT}")

    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and file_hash(dest) == file_hash(PLUGIN_ARTIFACT):
        note(f"plugin   unchanged  {dest}")
    else:
        shutil.copy2(PLUGIN_ARTIFACT, dest)
        note(f"plugin   copied     {PLUGIN_ARTIFACT} -> {dest}")
    return dest


def run_ffmpeg(track: Path) -> None:
    track.parent.mkdir(parents=True, exist_ok=True)
    command = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "anullsrc=r=44100:cl=mono",
        "-t", str(TRACK_SECONDS),
        "-metadata", "title=Rig Fixture Track",
        "-metadata", "artist=LX Lyrics Rig",
        "-metadata", "album=Rig Fixtures",
        "-c:a", "flac",
        str(track),
    ]
    subprocess.run(command, check=True)


def check_track(track: Path) -> None:
    """Sanity-check the generated fixture: a FLAC the player will actually
    open. ffprobe when available, the format magic always."""
    with track.open("rb") as handle:
        magic = handle.read(4)
    if magic != b"fLaC":
        die(f"{track} is not a FLAC file (magic bytes {magic!r})")
    if shutil.which("ffprobe"):
        result = subprocess.run(
            ["ffprobe", "-hide_banner", "-v", "error", "-show_entries",
             "format=format_name", "-of", "default=nw=1:nk=1", str(track)],
            check=False, capture_output=True, text=True,
        )
        if result.returncode != 0 or result.stdout.strip() != "flac":
            die(f"ffprobe rejected {track}: {result.stdout.strip() or result.stderr.strip()}")
        note(f"fixture  checked    ffprobe fmt=flac, {track.stat().st_size} bytes")


def ensure_fixture(rig: Path, track_arg: str | None) -> Path:
    default_track = rig / "fixtures" / "track.flac"
    track = abspath(track_arg) if track_arg else default_track
    sidecar = track.with_suffix(".lrc")

    if track_arg and track != default_track:
        if not track.exists():
            die(f"track not found: {track} (omit --track to generate the rig fixture)")
        note(f"fixture  as-is      {track}")
        if not sidecar.exists():
            warn(f"no {sidecar.name} next to {track}; the app will find no lyrics")
        return track

    if not track.exists():
        if not shutil.which("ffmpeg"):
            die("ffmpeg is required to generate the rig fixture track")
        step(f"generating the rig fixture ({TRACK_SECONDS} s silent flac)")
        run_ffmpeg(track)
        check_track(track)
    else:
        note(f"fixture  present    {track}")

    if not sidecar.exists():
        write_if_changed(sidecar, LRC_TEXT)
        note(f"fixture  wrote      {sidecar}")
    return track


def ensure_appwrap(rig: Path) -> Path:
    """The AppPath wrapper, copied into the rig root (mode 755).

    The plugin execs its AppPath, so the file needs the exec bit - and mutating
    the checkout's bit (or referencing its path from fooyin.conf) would make the
    rig write to the repository. The rig root keeps its own copy instead."""
    dest = rig / "appwrap.py"
    if not APPWRAP.is_file():
        die(f"the AppPath wrapper is missing: {APPWRAP}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.is_file() or file_hash(dest) != file_hash(APPWRAP):
        shutil.copy2(APPWRAP, dest)
        note(f"wrapper  copied     {APPWRAP} -> {dest}")
    os.chmod(dest, 0o755)
    return dest


def ensure_conf(rig: Path) -> Path:
    conf = rig / "home" / ".config" / "fooyin" / "fooyin.conf"
    app_path = str(rig / "appwrap.py")
    content = (
        "[General]\n"
        "LogLevel=4\n"
        "[Engine]\n"
        "AudioOutput=ALSA|null\n"
        "[KeyboardShortcuts]\n"
        "LxLyrics.DesktopLyrics=Ctrl+I\n"
        "[LxLyrics]\n"
        f"AppPath={app_path}\n"
        "Enabled=false\n"
        "RememberState=false\n"
    )
    if write_if_changed(conf, content):
        note(f"fooyin   wrote      {conf}")
    else:
        note(f"fooyin   unchanged  {conf}")
    return conf


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="seed-config.py",
        description="Seed the Fooyin GUI test rig: private fooyin.conf, plugin copy, fixture track.",
        epilog="Idempotent; re-running only rewrites what differs.",
    )
    parser.add_argument("--rig-root", default=DEFAULT_RIG_ROOT,
                        help=f"rig root directory; must be absolute (default: {DEFAULT_RIG_ROOT})")
    parser.add_argument("--app", default=None,
                        help="lx-lyrics-app binary (default: repo build, else ~/.local/bin, "
                             "else the previously recorded <rig-root>/app-path)")
    parser.add_argument("--track", default=None,
                        help="fixture track to use as-is (default: generate <rig-root>/fixtures/track.flac)")
    args = parser.parse_args(argv)

    if not os.path.isabs(args.rig_root):
        die(f"the rig root must be an absolute path (got '{args.rig_root}')")
    rig = abspath(args.rig_root)
    if rig == Path("/"):
        die("refusing to use / as the rig root")

    app = resolve_app(args.app)
    if args.app and app is None:
        die(f"--app is not an executable file: {args.app}")
    if app is None:
        recorded = rig / "app-path"
        if recorded.is_file():
            candidate = recorded.read_text(encoding="utf-8").strip()
            if candidate and os.access(candidate, os.X_OK):
                app = candidate

    step(f"seeding rig root {rig}")
    appwrap = ensure_appwrap(rig)
    conf = ensure_conf(rig)
    plugin = ensure_plugin(rig)
    track = ensure_fixture(rig, args.track)

    if app is None:
        warn("no lx-lyrics-app binary found (build lyrics-app, or pass --app PATH)")
        app_record = rig / "app-path"
        if app_record.exists():
            app_record.unlink()
    else:
        write_if_changed(rig / "app-path", app + "\n")
        note(f"app      resolved   {app}")

    step("seeded")
    note(f"wrapper  {appwrap}")
    note(f"conf     {conf}")
    note(f"plugin   {plugin}")
    note(f"track    {track}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except subprocess.CalledProcessError as error:
        die(f"command failed ({error.returncode}): {' '.join(str(part) for part in error.cmd)}")
    except OSError as error:
        die(f"{error}")
