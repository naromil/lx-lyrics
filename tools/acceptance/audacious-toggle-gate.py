#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 LX Lyrics contributors.
"""Acceptance gate for the "audacious idle toggle + orphan sweep" round.

The supervisor's oracle, not the implementer's. Every check reads an artifact:
real processes in /proc, exit codes, the compiled state machine put through
tools/acceptance/session_contract_test.c, clang-format, ctest.

    python3 tools/acceptance/audacious-toggle-gate.py [--no-build]

Exit 0 = the round is acceptable. Prints one line per check.
"""
import argparse
import hashlib
import os
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_BUILD = os.path.join(ROOT, "lyrics-app", "build")
APP_BIN = os.path.join(APP_BUILD, "lx-lyrics-app")
AUD_BUILD = os.path.join(ROOT, "plugins", "audacious", "build")
AUD_SRC = os.path.join(ROOT, "plugins", "audacious", "src")
ENV = dict(os.environ, QT_QPA_PLATFORM="offscreen")
TIMEOUT = 900

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + ((" :: " + detail) if detail and not ok else ""))
    return ok


def run(cmd, cwd=ROOT, timeout=TIMEOUT, env=None):
    return subprocess.run(cmd, cwd=cwd, timeout=timeout, env=env or ENV,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


# ---------------------------------------------------------------- processes

def pids_by_pgrp(pgrp):
    """Processes in @pgrp whose argv[0] basename is lx-lyrics-app. The decoy
    spawns real children (sleep), so the process group is not enough."""
    out = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % entry, "rb") as fh:
                fields = fh.read().rsplit(b")", 1)[1].split()
            if int(fields[2]) != pgrp:  # pgrp is the third field after comm
                continue
            with open("/proc/%s/cmdline" % entry, "rb") as fh:
                argv0 = fh.read().split(b"\0")[0]
            if os.path.basename(argv0.decode("utf-8", "replace")) == "lx-lyrics-app":
                out.append(int(entry))
        except (OSError, IndexError, ValueError):
            continue
    return out


def alive(pid):
    try:
        with open("/proc/%d/stat" % pid, "rb") as fh:
            state = fh.read().rsplit(b")", 1)[1].split()[0]
        return state not in (b"Z", b"X", b"x")
    except OSError:
        return False


def ppid_of(pid):
    try:
        with open("/proc/%d/status" % pid) as fh:
            for line in fh:
                if line.startswith("PPid:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return -1


def spawn_orphan(script, with_arg=True):
    """Double-fork + setsid so the grandchild is reparented away from us.
    Returns the pid of the (only) process in its new process group."""
    argv = ["lx-lyrics-app", "-c", script] + (["--player-feed"] if with_arg else [])
    pid = os.fork()
    if pid == 0:
        try:
            os.setsid()
            if os.fork() > 0:
                os._exit(0)
            os.execv("/bin/sh", argv)
        except BaseException:
            os._exit(127)
    os.waitpid(pid, 0)
    for _ in range(100):
        found = pids_by_pgrp(pid)
        if found:
            return found[0]
        time.sleep(0.02)
    raise RuntimeError("orphan decoy did not appear")


def _spawn_argv(argv):
    """fork/exec with an arbitrary argv[0] (subprocess cannot fake it on Linux)."""
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(r)
            os.execv("/bin/sh", argv)
        except BaseException:
            os._exit(127)
    os.close(w)
    return pid


# ---------------------------------------------------------------- checks

def check_builds():
    ok = True
    for build, cwd in ((APP_BUILD, os.path.join(ROOT, "lyrics-app")),
                       (AUD_BUILD, os.path.join(ROOT, "plugins", "audacious"))):
        r = run(["cmake", "--build", build], cwd=cwd)
        ok = check("build %s" % os.path.relpath(build, ROOT), r.returncode == 0,
                   r.stdout[-1500:]) and ok
    return ok


def check_suites():
    ok = True
    for name, build, expect in (("app", APP_BUILD, 6), ("audacious", AUD_BUILD, 1)):
        r = run(["ctest", "--test-dir", build])
        ok = check("ctest %s (%d suites)" % (name, expect),
                   r.returncode == 0 and ("100% tests passed" in r.stdout),
                   r.stdout[-2000:]) and ok
    return ok


def check_session_contract():
    tmp = tempfile.mktemp(suffix=".bin")
    r = run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", AUD_SRC,
             os.path.join(ROOT, "tools", "acceptance", "session_contract_test.c"),
             os.path.join(AUD_SRC, "session.c"), "-o", tmp])
    if not check("session.c compiles against the dictated header", r.returncode == 0, r.stdout[-1500:]):
        return False
    try:
        r = run([tmp])
        return check("session state machine contract", r.returncode == 0, r.stdout[-1500:])
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def check_no_disable():
    path = os.path.join(AUD_SRC, "plugin.cc")
    try:
        text = open(path).read()
    except OSError as exc:
        return check("plugin.cc readable", False, str(exc))
    return check("plugin.cc never disables the plugin (no aud_plugin_enable)",
                 "aud_plugin_enable" not in text,
                 "found %d occurrence(s)" % text.count("aud_plugin_enable"))


def check_orphan_sweep():
    if not os.access(APP_BIN, os.X_OK):
        return check("app binary present", False, APP_BIN)
    deaf = "trap '' TERM; while :; do sleep 0.2; done"
    polite = "while :; do sleep 0.2; done"
    decoys = {}
    try:
        o1 = spawn_orphan(deaf)        # orphan, ignores SIGTERM -> needs SIGKILL
        o2 = spawn_orphan(polite)      # orphan, dies on SIGTERM
        live = _spawn_argv(["lx-lyrics-app", "-c", polite, "--player-feed"])  # live parent
        wrong = spawn_orphan(polite, with_arg=False)  # orphan, but not a feed child
        decoys = dict(deaf=o1, polite=o2, live=live, wrong=wrong)
        time.sleep(0.3)
        for name, pid in decoys.items():
            if not alive(pid):
                return check("decoy %s alive before the sweep" % name, False, "pid %d" % pid)

        p = subprocess.Popen([APP_BIN, "--player-feed"], cwd=os.path.join(ROOT, "lyrics-app"),
                             stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, env=ENV, text=True)
        time.sleep(1.0)
        try:
            p.stdin.close()
        except OSError:
            pass
        _, err = p.communicate(timeout=15)

        deadline = time.time() + 5.0
        while time.time() < deadline and (alive(o1) or alive(o2)):
            time.sleep(0.05)
        ok = True
        ok = check("orphan that ignores SIGTERM is killed", not alive(o1),
                   "pid %d still alive" % o1) and ok
        ok = check("orphan that honours SIGTERM is killed", not alive(o2),
                   "pid %d still alive" % o2) and ok
        ok = check("child of a live parent is left alone", alive(live),
                   "pid %d was killed" % live) and ok
        ok = check("same name without --player-feed is left alone", alive(wrong),
                   "pid %d was killed" % wrong) and ok
        reaped = [line for line in err.splitlines()
                  if any(("pid=%d" % pid) in line or ("pid %d" % pid) in line
                         for pid in (o1, o2))]
        ok = check("the sweep logs what it reaped", bool(reaped), err[-1000:]) and ok
        return ok
    finally:
        for pid in decoys.values():
            for sig in (signal.SIGCONT, signal.SIGKILL):
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass


def check_format():
    r = run(["git", "diff", "--name-only", "main", "--", "*.c", "*.cc", "*.h"])
    files = [f for f in r.stdout.split() if os.path.exists(os.path.join(ROOT, f))]
    if not files:
        return check("clang-format (no changed C/C++ files)", True)
    r = run(["clang-format", "--dry-run", "-Werror"] + files)
    return check("clang-format --dry-run -Werror on changed C/C++ (%d file(s))" % len(files),
                 r.returncode == 0, r.stdout[-1500:])


def check_protocol_regressions():
    ok = True
    r = subprocess.run([APP_BIN, "--player-feed"], input='{"v":2,"action":"nonsense"}\n',
                       cwd=os.path.join(ROOT, "lyrics-app"), env=ENV, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    ok = check("a well-formed line with an unknown action is a protocol error (exit 1)",
               r.returncode == 1, "exit=%d out=%s" % (r.returncode, r.stdout[-600:])) and ok
    r = run(["timeout", "3", APP_BIN, "--demo"], cwd=os.path.join(ROOT, "lyrics-app"), timeout=60)
    ok = check("--demo runs headless until the timeout (exit 124)",
               r.returncode == 124, "exit=%d out=%s" % (r.returncode, r.stdout[-600:])) and ok
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    me = os.path.abspath(__file__)
    print("gate sha256 " + hashlib.sha256(open(me, "rb").read()).hexdigest())

    ok = True
    if not args.no_build:
        ok = check_builds() and ok
    ok = check_suites() and ok
    ok = check_session_contract() and ok
    ok = check_no_disable() and ok
    ok = check_orphan_sweep() and ok
    ok = check_format() and ok
    ok = check_protocol_regressions() and ok

    passed = sum(1 for _, p, _ in results if p)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
