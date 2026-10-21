#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 LX Lyrics contributors.
"""Acceptance gate for the "audacious global shortcut" round.

The supervisor's oracle, not the implementer's. Reads artifacts: real helper
processes and their pipes, the compiled hotkey adapter put through
tools/acceptance/hotkey_contract_test.c, the round-1 gate re-run, exit codes.

    python3 tools/acceptance/audacious-hotkey-gate.py [--no-build]

Exit 0 = the round is acceptable. Prints one line per check.
"""
import argparse
import hashlib
import os
import select
import stat
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_BUILD = os.path.join(ROOT, "lyrics-app", "build")
APP_BIN = os.path.join(APP_BUILD, "lx-lyrics-app")
HELPER = os.path.join(APP_BUILD, "lx-lyrics-hotkey")
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


def check_builds():
    ok = True
    for build, cwd in ((APP_BUILD, os.path.join(ROOT, "lyrics-app")),
                       (AUD_BUILD, os.path.join(ROOT, "plugins", "audacious"))):
        r = run(["cmake", "--build", build], cwd=cwd)
        ok = check("build %s" % os.path.relpath(build, ROOT), r.returncode == 0,
                   r.stdout[-1500:]) and ok
    ok = check("the helper binary is built", os.access(HELPER, os.X_OK), HELPER) and ok
    return ok


def check_suites():
    ok = True
    for name, build, expect in (("app", APP_BUILD, 7), ("audacious", AUD_BUILD, 3)):
        listing = run(["ctest", "--test-dir", build, "-N"])
        found = listing.stdout.count("Test #")
        ok = check("ctest %s has %d suites" % (name, expect), found == expect,
                   "ctest -N lists %d" % found) and ok
        r = run(["ctest", "--test-dir", build])
        ok = check("ctest %s passes" % name, r.returncode == 0 and "100% tests passed" in r.stdout,
                   r.stdout[-2000:]) and ok
    return ok


def check_hotkey_contract():
    tmp = "/tmp/lx-hotkey-contract-%d" % os.getpid()
    r = run(["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-I", AUD_SRC,
             os.path.join(ROOT, "tools", "acceptance", "hotkey_contract_test.c"),
             os.path.join(AUD_SRC, "hotkey.c"), "-o", tmp, "-lpthread"])
    if not check("hotkey.c compiles against the dictated header", r.returncode == 0, r.stdout[-1500:]):
        return False
    try:
        r = run([tmp], timeout=120)
        return check("hotkey adapter contract", r.returncode == 0, r.stdout[-1500:])
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def check_round1_gate():
    r = run([sys.executable, os.path.join(ROOT, "tools", "acceptance", "audacious-toggle-gate.py"),
             "--no-build"], timeout=TIMEOUT)
    return check("round-1 gate (idle toggle + orphan sweep) still green", r.returncode == 0,
                 r.stdout[-1500:])


def check_helper_degrades():
    """With no app id the portal refuses; the helper must say so and exit 0,
    not crash or hang — that is the terminal-launched-player case."""
    if not os.access(HELPER, os.X_OK):
        return check("the helper is runnable", False, "%s missing" % HELPER)
    read_fd, write_fd = os.pipe()
    try:
        process = subprocess.Popen([HELPER, "--pipe-fd", str(write_fd), "--accelerator", "CTRL+ALT+l"],
                                   cwd=os.path.join(ROOT, "lyrics-app"), env=ENV,
                                   pass_fds=(write_fd,), stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True)
        os.close(write_fd)
        write_fd = None
        first = b""
        deadline = time.time() + 15
        while time.time() < deadline:
            ready, _, _ = select.select([read_fd], [], [], 0.2)
            if ready:
                chunk = os.read(read_fd, 4096)
                if not chunk:
                    break
                first += chunk
                if b"\n" in first:
                    break
            if process.poll() is not None:
                break
        line = first.split(b"\n")[0].decode("utf-8", "replace")
        exited = process.poll()
        if exited is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
        ok = check("the helper reports 'unavailable' when it has no app id",
                   line.startswith("unavailable"), "first line=%r" % line)
        return ok
    finally:
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)


def check_helper_arg_errors():
    if not os.access(HELPER, os.X_OK):
        return check("the helper is runnable for the usage check", False, "%s missing" % HELPER)
    r = run([HELPER], cwd=os.path.join(ROOT, "lyrics-app"), timeout=60)
    ok = check("the helper without --pipe-fd fails loudly (exit 2)",
               r.returncode == 2 and "pipe-fd" in r.stdout, "exit=%d out=%s"
               % (r.returncode, r.stdout[-400:]))
    return ok


def check_wiring():
    path = os.path.join(AUD_SRC, "plugin.cc")
    try:
        text = open(path).read()
    except OSError as exc:
        return check("plugin.cc readable", False, str(exc))
    ok = True
    for symbol in ("lx_hotkey_start(", "lx_hotkey_stop("):
        ok = check("plugin.cc uses %s" % symbol.rstrip("("), symbol in text) and ok
    ok = check("plugin.cc still never disables the plugin",
               "aud_plugin_enable" not in text) and ok
    return ok


def check_format():
    r = run(["git", "diff", "--name-only", "main", "--", "*.c", "*.cc", "*.h"])
    files = [f for f in r.stdout.split() if os.path.exists(os.path.join(ROOT, f))]
    if not files:
        return check("clang-format (no changed C/C++ files)", True)
    r = run(["clang-format", "--dry-run", "-Werror"] + files)
    return check("clang-format --dry-run -Werror on changed C/C++ (%d file(s))" % len(files),
                 r.returncode == 0, r.stdout[-1500:])


def check_install_ships_helper():
    script = open(os.path.join(ROOT, "tools", "install.sh")).read()
    return check("install.sh installs lx-lyrics-hotkey next to the app",
                 "lx-lyrics-hotkey" in script, "not mentioned in tools/install.sh")


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
    ok = check_hotkey_contract() and ok
    ok = check_helper_degrades() and ok
    ok = check_helper_arg_errors() and ok
    ok = check_wiring() and ok
    ok = check_install_ships_helper() and ok
    ok = check_format() and ok
    ok = check_round1_gate() and ok

    passed = sum(1 for _, p, _ in results if p)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
