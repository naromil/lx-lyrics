#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 LX Lyrics contributors.
#
# The Fooyin test rig's AppPath: a transparent tee around lx-lyrics-app.
#
#   appwrap.py [--app PATH] [--rig-root DIR] [--feed-log PATH] [--player-feed]
#
# Fooyin's LxLyrics plugin spawns its [LxLyrics] AppPath value as a direct
# child with a single argument (--player-feed) and speaks the v2 wire on that
# child's stdin/stdout (docs/protocol.md §2). Pointing AppPath at this wrapper
# instead of at the app itself gives the rig a log of every line of that
# conversation while keeping the transport byte-for-byte intact:
#
#   host -> app   read from our stdin,  written to the child's stdin,  logged as "<< <line>"
#   app  -> host   read from the child's stdout, forwarded to our stdout, logged as ">> <line>"
#   app  -> stderr inherited untouched: the plugin already drains it and logs
#       each line into fooyin.log prefixed "[LX Lyrics app]", so a second tee
#       here would only duplicate it.
#
# Both directions are read BY LINE — a partial line never reaches the peer as
# a partial JSON message — and each direction has its own thread, so a slow or
# blocked reader on one side can never stall the other (the plugin writes the
# `hello` handshake immediately after the spawn, before any state push).
#
# Lifetime: the app dies when its stdin reaches EOF (§2), so our stdin EOF
# (the plugin closed the write channel) closes the child's stdin and the
# wrapper exits with the child's own exit code. If the child does not exit,
# it is terminated, then killed, and the wrapper still exits with that code.
#
# Log path: <rig-root>/feed.log, i.e. $LX_RIG_ROOT/feed.log as exported by
# rig.sh (the wrapper itself is spawned by Fooyin, which got that variable
# from the rig). One log line per wire line, in arrival order.
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RIG_ROOT = "/tmp/lx-lyrics-gui-rig"
STOP_GRACE_SECONDS = 2.0
READER_JOIN_SECONDS = 2.0
EOF_GRACE_SECONDS = 1.5


def warn(message: str) -> None:
    sys.stderr.write(f"appwrap: {message}\n")
    sys.stderr.flush()


def recorded_app(rig_root: str) -> str | None:
    """The app path an earlier seed-config.py recorded in <rig-root>/app-path."""
    try:
        text = Path(rig_root, "app-path").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    lines = text.strip().splitlines()
    return lines[0].strip() if lines else None


def default_app(rig_root: str) -> str | None:
    """The app to run when neither --app nor $LX_RIG_APP is set.

    rig.sh exports LX_RIG_APP, so the plugin's child normally gets it from the
    environment; <rig-root>/app-path is what a previous seed-config.py recorded
    and keeps a hand-run wrapper working. The repo build and the user install
    are the last two candidates."""
    candidates = []
    recorded = recorded_app(rig_root)
    if recorded:
        candidates.append(recorded)
    # N11: REPO_ROOT is derived from __file__, which is the checkout only when
    # this script runs from tools/gui-test/. In the rig root's copy it points
    # somewhere meaningless, so the repo candidate is offered only when the
    # checkout layout is really there.
    if (REPO_ROOT / "tools" / "gui-test" / "appwrap.py").is_file():
        candidates.append(str(REPO_ROOT / "lyrics-app" / "build" / "lx-lyrics-app"))
    candidates.append(os.path.join(os.path.expanduser("~"), ".local", "bin", "lx-lyrics-app"))
    for candidate in candidates:
        if candidate and os.access(candidate, os.X_OK):
            return candidate
    return None


class FeedLog:
    """Append-only wire log. A failure to open it never stops the session —
    the tee is an observer, not part of the transport."""

    def __init__(self, path: Path) -> None:
        self.lock = threading.Lock()
        self.handle = None
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.handle = open(path, "ab", buffering=0)
        except OSError as error:  # pragma: no cover - depends on the filesystem
            warn(f"cannot open {path} for the feed log: {error}")

    def line(self, prefix: bytes, line: bytes) -> None:
        payload = prefix + line.rstrip(b"\r\n") + b"\n"
        with self.lock:
            if self.handle is None:
                return
            try:
                self.handle.write(payload)
            except (OSError, ValueError):
                # ValueError: close() held this same lock and closed the
                # handle; the log is an observer, not part of the wire.
                pass

    def close(self) -> None:
        with self.lock:
            if self.handle is None:
                return
            try:
                self.handle.close()
            except (OSError, ValueError):
                pass
            self.handle = None


def pump_stdin_to_child(child: subprocess.Popen, log: FeedLog, eof: threading.Event) -> None:
    """Our stdin (host -> app) into the child's stdin, logged as '<< '."""
    source = sys.stdin.buffer
    try:
        while True:
            line = source.readline()
            if not line:
                break
            log.line(b"<< ", line)
            try:
                child.stdin.write(line)
                child.stdin.flush()
            except (BrokenPipeError, ValueError, OSError):
                break
    finally:
        # §2: EOF on the child's stdin is the app's quit request.
        try:
            child.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        eof.set()


def pump_child_to_stdout(child: subprocess.Popen, log: FeedLog) -> None:
    """The child's stdout (app -> host) onto our stdout, logged as '>> '."""
    sink = sys.stdout.buffer
    try:
        while True:
            line = child.stdout.readline()
            if not line:
                break
            log.line(b">> ", line)
            try:
                sink.write(line)
                sink.flush()
            except (BrokenPipeError, OSError):
                # The host stopped draining us: keep reading the child so it
                # does not block, and drop the undeliverable lines.
                continue
    finally:
        try:
            sink.flush()
        except (BrokenPipeError, OSError):
            pass


def stop_child(child: subprocess.Popen) -> None:
    if child.poll() is not None:
        return
    try:
        child.terminate()
    except OSError:
        return
    try:
        child.wait(timeout=STOP_GRACE_SECONDS)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        child.kill()
        child.wait(timeout=STOP_GRACE_SECONDS)
    except (OSError, subprocess.TimeoutExpired):
        pass


def exit_code(returncode: int) -> int:
    # The plugin reports a signal death as a crash (QProcess::CrashExit) and
    # only reads the code on a normal exit; map it the shell way all the same.
    return returncode if returncode >= 0 else 128 - returncode


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="appwrap.py",
        description="Tee the lx-lyrics-app v2 feed into <rig-root>/feed.log.",
        epilog="Unknown arguments (Fooyin passes --player-feed) are forwarded to the app.",
    )
    parser.add_argument("--app", default=None,
                        help="lx-lyrics-app binary (default: $LX_RIG_APP, then <rig-root>/app-path, "
                             "then the repo build, then ~/.local/bin)")
    parser.add_argument("--rig-root", default=None,
                        help=f"rig root holding feed.log; must be absolute "
                             f"(default: $LX_RIG_ROOT, then {DEFAULT_RIG_ROOT})")
    parser.add_argument("--feed-log", default=None,
                        help="feed log path (default: <rig-root>/feed.log)")
    args, passthrough = parser.parse_known_args(argv)

    rig_root = args.rig_root or os.environ.get("LX_RIG_ROOT") or DEFAULT_RIG_ROOT
    if not os.path.isabs(rig_root):
        warn(f"the rig root must be an absolute path (got '{rig_root}')")
        return 2

    app = args.app or os.environ.get("LX_RIG_APP") or default_app(rig_root)
    if not app:
        warn("no lx-lyrics-app binary: pass --app, set LX_RIG_APP, or build lyrics-app")
        return 2
    if not os.access(app, os.X_OK):
        warn(f"not an executable file: {app}")
        return 2

    feed_log = Path(args.feed_log) if args.feed_log else Path(rig_root) / "feed.log"

    child_args = [app] + list(passthrough)
    if "--player-feed" not in child_args:
        child_args.append("--player-feed")

    log = FeedLog(feed_log)
    environment = os.environ.copy()
    environment["LX_RIG_ROOT"] = rig_root

    child = subprocess.Popen(  # noqa: S603 - argv array, never a shell (§8)
        child_args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=None,  # the plugin's own stderr drain already tees it into fooyin.log
        env=environment,
    )

    stdin_eof = threading.Event()
    threads = [
        threading.Thread(target=pump_stdin_to_child, args=(child, log, stdin_eof),
                         name="feed-in", daemon=True),
        threading.Thread(target=pump_child_to_stdout, args=(child, log),
                         name="feed-out", daemon=True),
    ]
    for thread in threads:
        thread.start()

    stop_event = threading.Event()
    stop_signal: list[int] = []

    def on_signal(signum: int, _frame: object) -> None:
        # Only flag it. child.wait() takes Popen._waitpid_lock, a non-reentrant
        # lock the main thread holds inside child.wait(timeout=0.2), so calling
        # wait()/poll() (or stop_child) from the handler deadlocks whenever the
        # signal lands in that window. The main loop below does the stopping.
        stop_signal.append(signum)
        stop_event.set()

    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, on_signal)

    # Wait for the child, and on our stdin EOF give it EOF_GRACE_SECONDS to
    # honour the quit request before terminating it - a child that ignores the
    # EOF must not keep the wrapper (the plugin's direct child) alive.
    deadline: float | None = None
    try:
        while True:
            if stop_event.is_set():
                warn(f"signal {stop_signal[0]}: terminating {os.path.basename(app)}")
                stop_child(child)
                break
            try:
                child.wait(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                if not stdin_eof.is_set():
                    continue
                if deadline is None:
                    deadline = time.monotonic() + EOF_GRACE_SECONDS
                elif time.monotonic() >= deadline:
                    warn(f"stdin is closed but {os.path.basename(app)} did not exit; terminating it")
                    stop_child(child)
                    break
    finally:
        stop_child(child)
        reader_deadline = time.monotonic() + READER_JOIN_SECONDS
        for thread in threads:
            thread.join(max(0.0, reader_deadline - time.monotonic()))
        log.close()

    return exit_code(child.returncode if child.returncode is not None else 0)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        raise SystemExit(130) from None
