#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 LX Lyrics contributors.
"""Acceptance gate for round 3: the global shortcut in the C adapters
(DeaDBeeF and VLC).

Same oracle shape as the audacious rounds: the adapter's own transport is put
through the supervisor's contract test, the project suites must stay green, and
the earlier gates must not regress.

    python3 tools/acceptance/shortcut-c-adapters-gate.py [--no-build]
"""
import argparse
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_BUILD = os.path.join(ROOT, "lyrics-app", "build")
ENV = dict(os.environ)
TIMEOUT = 900

ADAPTERS = {
    "deadbeef": os.path.join(ROOT, "plugins", "deadbeef"),
    "vlc": os.path.join(ROOT, "plugins", "vlc"),
}

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + ((" :: " + detail) if detail and not ok else ""))
    return ok


def run(cmd, cwd=ROOT, timeout=TIMEOUT):
    return subprocess.run(cmd, cwd=cwd, timeout=timeout, env=ENV,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def check_builds():
    ok = True
    for name, project in ADAPTERS.items():
        build = os.path.join(project, "build")
        r = run(["cmake", "--build", build], cwd=project)
        ok = check("build plugins/%s" % name, r.returncode == 0, r.stdout[-1200:]) and ok
    return ok


def check_suites():
    ok = True
    for name, project in ADAPTERS.items():
        build = os.path.join(project, "build")
        r = run(["ctest", "--test-dir", build])
        ok = check("ctest plugins/%s" % name,
                   r.returncode == 0 and "100% tests passed" in r.stdout, r.stdout[-1500:]) and ok
        listing = run(["ctest", "--test-dir", build, "-N"])
        ok = check("plugins/%s runs a hotkey suite" % name, "hotkey_test" in listing.stdout,
                   listing.stdout[-600:]) and ok
    return ok


def check_contracts():
    ok = True
    for name, project in ADAPTERS.items():
        src = os.path.join(project, "src")
        binary = "/tmp/lx-hotkey-contract-%s-%d" % (name, os.getpid())
        r = run(["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-I", src,
                 os.path.join(ROOT, "tools", "acceptance", "hotkey_contract_test.c"),
                 os.path.join(src, "hotkey.c"), "-o", binary, "-lpthread"])
        if not check("plugins/%s hotkey.c compiles against the contract header" % name,
                     r.returncode == 0, r.stdout[-1200:]):
            ok = False
            continue
        try:
            r = run([binary], timeout=120)
            ok = check("plugins/%s hotkey transport contract" % name, r.returncode == 0,
                       r.stdout[-1200:]) and ok
        finally:
            if os.path.exists(binary):
                os.unlink(binary)
    return ok


def check_wiring():
    ok = True
    for name, project in ADAPTERS.items():
        path = os.path.join(project, "src", "plugin.c")
        try:
            text = open(path).read()
        except OSError as exc:
            ok = check("plugins/%s/src/plugin.c readable" % name, False, str(exc)) and ok
            continue
        for symbol in ("lx_hotkey_start(", "lx_hotkey_stop("):
            ok = check("plugins/%s wires %s" % (name, symbol.rstrip("(")),
                       symbol in text) and ok
    return ok


def check_format():
    r = run(["git", "diff", "--name-only", "main", "--", "*.c", "*.h"])
    files = [f for f in r.stdout.split() if os.path.exists(os.path.join(ROOT, f))]
    if not files:
        return check("clang-format (no changed C/C++ files)", True)
    r = run(["clang-format", "--dry-run", "-Werror"] + files)
    return check("clang-format --dry-run -Werror on changed C (%d file(s))" % len(files),
                 r.returncode == 0, r.stdout[-1500:])


def check_earlier_gates():
    ok = True
    for gate in ("audacious-toggle-gate.py", "audacious-hotkey-gate.py"):
        r = run([sys.executable, os.path.join(ROOT, "tools", "acceptance", gate), "--no-build"])
        ok = check("%s still green" % gate, r.returncode == 0, r.stdout[-1200:]) and ok
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()
    print("gate sha256 " + hashlib.sha256(open(os.path.abspath(__file__), "rb").read()).hexdigest())

    ok = True
    if not args.no_build:
        ok = check_builds() and ok
    ok = check_suites() and ok
    ok = check_contracts() and ok
    ok = check_wiring() and ok
    ok = check_format() and ok
    ok = check_earlier_gates() and ok

    passed = sum(1 for _, p, _ in results if p)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
