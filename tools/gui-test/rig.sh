#!/usr/bin/env bash
#
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 LX Lyrics contributors.
#
# Headless GUI test rig for the Fooyin adapter.
#
#   tools/gui-test/rig.sh start|stop|status|restart [--rig-root DIR] [--app PATH] [--track PATH]
#   tools/gui-test/rig.sh stop --purge
#   tools/gui-test/rig.sh fooyin <fooyin args...>   # the fooyin CLI against this rig
#   tools/gui-test/rig.sh key <key args...>         # a key chord to the rig's window
#
# Brings up a real Fooyin (0.12.6) and a real lx-lyrics-app that cannot see or
# disturb the user's session, so the session-lifecycle and window-state bugs
# the plugin only shows against a real player can be reproduced and observed:
#
#   * a private X display: Xvfb picks a free number itself (`-displayfd`),
#     never a hardcoded :99, and QT_QPA_PLATFORM=xcb with WAYLAND_DISPLAY and
#     XDG_SESSION_TYPE dropped keeps the child off the user's Wayland session.
#     No window manager is needed: a bare Xvfb maps the Fooyin main window.
#   * a private HOME and every XDG_* directory under <rig-root>/home, a private
#     D-Bus session bus, a private XDG_RUNTIME_DIR and a private TMPDIR
#     (<rig-root>/tmp). HOME matters beyond the XDG variables: Fooyin's
#     per-user plugin directory is $HOME/.local/lib/fooyin/plugins (NOT
#     $XDG_DATA_HOME) and its database and state file are HOME-derived. TMPDIR
#     matters just as much, and XDG_RUNTIME_DIR does NOT cover it: fooyin's
#     single-instance socket comes from KDSingleApplication, which resolves it
#     through QDir::tempPath() as kdsingleapp-<user>-fooyin. With the ambient
#     TMPDIR that socket is per-user, so the rig's fooyin would share it with
#     the user's own player; pointing TMPDIR at <rig-root>/tmp puts the socket
#     inside the rig, which is what pins `fooyin -p <file>` to *this* instance
#     and keeps the user's player out of the loop.
#   * the real plugin module, copied by seed-config.py into the private HOME.
#   * the wire: [LxLyrics] AppPath points at <rig-root>/appwrap.py (a copy of
#     tools/gui-test/appwrap.py made on every start), which *runs* the app as a
#     child - it does not exec it - and tees every protocol line into
#     <rig-root>/feed.log.
#
# What start leaves behind (all under <rig-root>, default
# ${LX_RIG_ROOT:-/tmp/lx-lyrics-gui-rig}):
#
#   rig.env    sourceable (every entry is exported): HOME, XDG_* (incl.
#              XDG_RUNTIME_DIR), TMPDIR, DISPLAY, DBUS_SESSION_BUS_ADDRESS,
#              LX_RIG_ROOT, LX_RIG_APP, the fixture path and the Fooyin version
#   .rig-root  the marker that proves this directory is a rig root: the
#              environment sweep and --purge run only for a marked root (a root
#              written by the previous revision is still recognised through its
#              rig.env)
#   display    the Xvfb display number, e.g. 2 for :2
#   pids       "<name> <pid> <binary> <starttime>" per line - the only
#              processes stop may signal, and only while all four still match
#              /proc/<pid> (see pid_is): the start time defeats pid reuse
#   appwrap.py the AppPath wrapper, copied here on every start so a checkout
#              without the exec bit still works and the repository is never
#              written to; [LxLyrics] AppPath points at this copy
#   user-files.mtimes
#              mtimes (or "absent") of the operator's own fooyin/lx-lyrics
#              files, re-checked by stop and status so a leak out of the rig
#              root is named instead of passing silently
#   tmp/       the rig's private TMPDIR: fooyin's single-instance socket
#              (kdsingleapp-<user>-fooyin) is created here, so the rig's CLI
#              and the rig's fooyin find each other and nothing else does
#   fooyin.log fooyin's log (the plugin's own lines land here, including the
#              app's stderr prefixed "[LX Lyrics app]")
#   feed.log   the v2 wire, "<< " host->app and ">> " app->host
#   xvfb.log, dbus.log
#
# Teardown never signals anything by name. A recorded pid is signalled only
# while /proc/<pid> still proves it is that process: the recorded start time
# matches field 22 of /proc/<pid>/stat and /proc/<pid>/exe resolves to the
# recorded binary (a basename match is not enough). On top of that, and only
# when the rig root carries its .rig-root marker, the rig sweeps
# /proc/*/environ once and signals only pids whose *launch* environment carries
# a path equal to or under the rig root - proof of rig ownership, not proof the
# rig started them. Anything whose path belongs to a nested rig root, this
# shell or its ancestors, or a shell interpreter is skipped and named. The rig
# never touches ~/.config/fooyin, ~/.config/lx-lyrics or
# ~/.local/lib/fooyin/plugins; stop re-checks their mtimes (user-files.mtimes)
# against the values start recorded and warns when one moved.
#
# Playing the fixture: the ALSA|null output is unthrottled, so playback runs
# well ahead of realtime and no assertion may assume realtime playback.
# Deterministic positions come from pause-stepping, with the seek values in
# milliseconds (fooyin parses them with std::stoull, so "-R 150s" is 150 ms):
#
#   fooyin -p <track>; sleep 2; fooyin -u; fooyin -R 600000; fooyin -F 150000
#
#   -u pauses, the oversized -R clamps the position to 0, -F lands on exactly
#   150000 ms, and the pause means the app renders that frame until the next
#   command.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
seed_script="$script_dir/seed-config.py"
appwrap_script="$script_dir/appwrap.py"

command_name=""
rig_root="${LX_RIG_ROOT:-/tmp/lx-lyrics-gui-rig}"
app_flag=""
track_flag=""
purge=0
force=0
passthrough_args=()
# Set by require_takeover_ok's plain mode when the root already is a rig root:
# the scaffold re-check then also tolerates the inventory that run left behind.
takeover_existing_rig=0
display_size="${LX_RIG_DISPLAY_SIZE:-1440x900x24}"
start_timeout="${LX_RIG_START_TIMEOUT:-45}"

# --- Helpers -----------------------------------------------------------------

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

step() {
    printf '\n==> %s\n' "$*"
}

note() {
    printf '    %s\n' "$*"
}

quote() {
    printf "'%s'" "${1//\'/\'\\\'\'}"
}

abspath() {
    if command -v realpath >/dev/null 2>&1; then
        realpath -m -- "$1"
    else
        case "$1" in
            /*) printf '%s\n' "$1" ;;
            *) printf '%s\n' "$PWD/$1" ;;
        esac
    fi
}

# --- Rig-root identity (F1, F25) ----------------------------------------------

# The operator's own home, independent of the ambient HOME: a shell that sourced
# rig.env has HOME pointing into the rig, so the rig must not take HOME at face
# value when it names the operator's session files.
user_home() {
    local home=""
    if command -v getent >/dev/null 2>&1; then
        home="$(getent passwd "$(id -u)" 2>/dev/null | cut -d: -f6 || true)"
    fi
    [ -n "$home" ] || home="${HOME:-}"
    printf '%s\n' "$home"
}

# Is <dir> a rig root? This revision proves it with the <dir>/.rig-root marker
# written on every start; a root created by the previous revision is proven by
# its generated rig.env, whose exported LX_RIG_ROOT line names that exact root
# (this is the migration path for roots that predate the marker).
is_rig_root() {
    local root="$1" line=""
    [ -e "$root/.rig-root" ] && return 0
    [ -s "$root/rig.env" ] || return 1
    line="$(sed -n "s/^export LX_RIG_ROOT='\(.*\)'\$/\1/p" "$root/rig.env" 2>/dev/null | head -n 1 || true)"
    [ -n "$line" ] && [ "$line" = "$root" ]
}

# The one root check every subcommand runs (start, status, stop, restart,
# fooyin, key, and the --purge path) before anything is created, signalled or
# removed (F1). A rig root may never be:
#   * "/" or the empty path;
#   * a top-level directory - the default root is /tmp/<name>, so ${root#/}
#     must contain a "/" (this is what refuses `--rig-root /tmp`);
#   * $HOME or the repository themselves;
#   * a directory that *contains* $HOME or the repository: that would make the
#     rig root an ancestor of the operator's session and of the checkout;
#   * a directory *inside* the repository: `start` would write a marker into the
#     sources and a later `stop --purge` would delete them (N5).
validate_rig_root() {
    local root="$1" home=""
    home="$(user_home)"
    case "$root" in
        ""|"/") die "refusing to use '$root' as the rig root" ;;
    esac
    case "${root#/}" in
        */*) ;;
        *) die "refusing to use the top-level directory '$root' as the rig root" ;;
    esac
    if [ -n "$home" ]; then
        [ "$root" = "$home" ] && die "refusing to use \$HOME ($home) as the rig root"
        case "$home/" in
            "$root"/*) die "refusing to use '$root' as the rig root: it contains \$HOME ($home)" ;;
        esac
    fi
    [ "$root" = "$repo_root" ] && die "refusing to use the repository ($repo_root) as the rig root"
    case "$repo_root/" in
        "$root"/*) die "refusing to use '$root' as the rig root: it contains the repository ($repo_root)" ;;
    esac
    case "$root/" in
        "$repo_root"/*) die "refusing to use '$root' as the rig root: it is inside the repository ($repo_root)" ;;
    esac
}

# NEW-1: may `start` take this directory over? A non-existent path, an empty
# readable directory and an existing rig root are fine; anything else is refused
# - including a directory whose contents cannot be listed, where "empty" cannot
# be verified at all. A mode-0300 directory hides its files from `ls`, so
# treating a failed listing as an empty one would let start mark - and a later
# `stop --purge` delete - a directory full of someone else's files.
#
# NEW-2: the check is not atomic against a writer racing start, so it is re-run
# immediately before seeding (--scaffold). By then the rig's own scaffold exists,
# so exactly those entries are accepted and any *other* entry added in the window
# between the two calls is still refused.
#
# NEW-A: the --scaffold mode deliberately does *not* take the is_rig_root early
# return - by then this run has just written the marker, and that marker must not
# be allowed to bypass the racing-writer inspection. It accepts what this run has
# created by then, and - when the root already was a rig root at the first check
# (which accepts one without inspection) - also the inventory the previous run
# left behind, so `start`/`restart` of a stopped root is not refused spuriously.
require_takeover_ok() {
    local mode="${1:-}" listing="" entry="" extra=""
    if [ -e "$rig_root" ] && [ ! -d "$rig_root" ]; then
        die "refusing to start at $rig_root: it exists and is not a directory"
    fi
    [ -d "$rig_root" ] || return 0
    if [ "$mode" != "--scaffold" ]; then
        if is_rig_root "$rig_root"; then
            takeover_existing_rig=1
            return 0
        fi
    fi
    listing="$(ls -A -- "$rig_root" 2>/dev/null)" \
        || die "refusing to start at $rig_root: cannot read the directory to verify it is empty"
    [ -n "$listing" ] || return 0
    if [ "$mode" != "--scaffold" ]; then
        die "refusing to start at $rig_root: the directory already exists, is not a rig root and is not empty; pick a fresh path, or point --rig-root at an existing rig root"
    fi
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        case "$entry" in
            .rig-root|appwrap.py|fixtures|home|run|tmp|user-files.mtimes) ;;
            app-path|dbus.log|display|display.tmp|feed.log|fooyin.log|pids|rig.env|xvfb.log)
                [ "$takeover_existing_rig" -eq 1 ] || extra="$entry"
                ;;
            *) extra="$entry" ;;
        esac
    done <<< "$listing"
    if [ -n "$extra" ]; then
        if [ "$takeover_existing_rig" -eq 1 ]; then
            die "refusing to start at $rig_root: something this run did not create appeared in it (found '$extra'); retry, or use a fresh path; if the entry is a leftover from an interrupted start, 'stop --purge' removes the whole root"
        fi
        die "refusing to start at $rig_root: it is not a rig root and holds something this run did not create (found '$extra'); pick a fresh path, or point --rig-root at an existing rig root"
    fi
    return 0
}

# The first rig root nested inside <root> (never <root> itself), or nothing
# (F3). `start` and `--purge` refuse such a root; `stop` instead skips that
# rig's processes (see foreign_rig_root), so a parent that predates the check can
# still be torn down without touching the child.
root_contains_nested_rig() {
    local root="$1" found="" dir=""
    while read -r found; do
        [ -n "$found" ] || continue
        dir="${found%/*}"
        [ "$dir" != "$root" ] || continue
        is_rig_root "$dir" || continue
        printf '%s\n' "$dir"
        return 0
    done < <(find "$root" -mindepth 1 \( -name .rig-root -o -name rig.env \) -print 2>/dev/null || true)
    return 0
}

# Does <path> - an environment value already known to be under $rig_root -
# actually belong to a *different* rig root nested inside $rig_root? Walks up
# from the path and prints the first rig root found before $rig_root; prints
# nothing when the path is ours or carries no marker (F3). Always exits 0 so the
# callers can assign its output under `set -e`.
foreign_rig_root() {
    local path="$1" parent=""
    while [ -n "$path" ] && [ "$path" != "/" ] && [ "$path" != "$rig_root" ]; do
        case "$path/" in
            "$rig_root"/*) ;;
            *) return 0 ;;
        esac
        if is_rig_root "$path"; then
            printf '%s\n' "$path"
            return 0
        fi
        parent="${path%/*}"
        [ -n "$parent" ] || parent="/"
        [ "$parent" != "$path" ] || break
        path="$parent"
    done
    return 0
}

# --- Recorded pids (F2) -------------------------------------------------------

# Field 22 of /proc/<pid>/stat: the process start time in clock ticks since boot,
# which identifies the *instance* (a recycled pid gets a different one).
# /proc/<pid>/stat's second field is comm, which may contain spaces and parens,
# so the fields are counted after the LAST ')': field 3 is then the first token.
proc_starttime() {
    local pid="$1" stat="" rest="" index=$((22 - 3))
    local -a fields=()
    [ -r "/proc/$pid/stat" ] || return 1
    IFS= read -r stat < "/proc/$pid/stat" 2>/dev/null || return 1
    rest="${stat##*)}"
    # shellcheck disable=SC2206  # splitting into the stat fields is the point
    fields=($rest)
    [ "${#fields[@]}" -gt "$index" ] || return 1
    printf '%s\n' "${fields[$index]}"
}

# How a record in <rig-root>/pids stands right now:
#   live       the recorded process: /proc/<pid> is up, its exe resolves to the
#              recorded binary, and the recorded start time still matches
#   dead       /proc/<pid> is gone (or a zombie)
#   stale      alive, but not that process any more - a recycled pid
#   unverified alive, but not provable: a legacy 3-field record without a start
#              time, or an unreadable /proc/<pid>/exe. Never signalled.
pid_verdict() {
    local pid="$1" binary="${2:-}" starttime="${3:-}" now="" want="" exe=""
    case "$pid" in
        ''|*[!0-9]*) printf 'dead\n'; return 0 ;;
    esac
    pid_alive "$pid" || { printf 'dead\n'; return 0; }
    if [ -z "$starttime" ] || [ -z "$binary" ]; then
        printf 'unverified\n'
        return 0
    fi
    now="$(proc_starttime "$pid")" || { printf 'unverified\n'; return 0; }
    if [ "$now" != "$starttime" ]; then
        printf 'stale\n'
        return 0
    fi
    want="$(readlink -f -- "$binary" 2>/dev/null || true)"
    exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    if [ -z "$want" ] || [ -z "$exe" ]; then
        printf 'unverified\n'
        return 0
    fi
    if [ "$exe" = "$want" ]; then printf 'live\n'; else printf 'stale\n'; fi
}

# May this recorded pid be signalled? Only when /proc/<pid> exists, the recorded
# start time matches, and /proc/<pid>/exe resolves to the recorded binary. A
# basename match is deliberately NOT enough: a stale record plus pid reuse by the
# operator's own fooyin must not make the rig terminate that player.
pid_is() {
    [ "$(pid_verdict "${1:-}" "${2:-}" "${3:-}")" = "live" ]
}

# One line per record in <rig-root>/pids, with its verdict:
#   <name> <pid> <binary> <starttime> <verdict>   (see pid_verdict)
# An absent field is printed as "-" so that read(1) - which collapses runs of
# IFS-whitespace and would swallow an empty column - keeps the columns aligned.
recorded_pid_records() {
    [ -s "$rig_root/pids" ] || return 0
    local name pid binary starttime
    while read -r name pid binary starttime; do
        [ -n "${name:-}" ] || continue
        [ -n "${pid:-}" ] || continue
        printf '%s %s %s %s %s\n' "$name" "$pid" "${binary:--}" "${starttime:--}" \
               "$(pid_verdict "$pid" "${binary:-}" "${starttime:-}")"
    done < "$rig_root/pids"
}

# Only the records proven live and ours, as
# <name> <pid> <binary> <starttime> (a live record always has both fields).
# Every site that may signal a recorded pid goes through this one helper, so a
# stale or unverifiable record can never become a signal target.
verified_recorded_pids() {
    local name pid binary starttime verdict
    while read -r name pid binary starttime verdict; do
        [ "$verdict" = "live" ] || continue
        printf '%s %s %s %s\n' "$name" "$pid" "$binary" "$starttime"
    done < <(recorded_pid_records)
}

# --- The operator's own files (F18) -------------------------------------------

# The user-session files the rig must never write. cmd_start records their
# mtimes (or "absent") in <rig-root>/user-files.mtimes; stop and status re-stat
# them and name any that moved (plan section 7: a leak out of the rig root has
# to fail loudly, not silently).
user_files() {
    local home=""
    home="$(user_home)"
    [ -n "$home" ] || return 0
    printf '%s\n' \
        "$home/.config/lx-lyrics/config.json" \
        "$home/.config/fooyin/fooyin.conf" \
        "$home/.local/lib/fooyin/plugins/fyplugin_lxlyrics.so"
}

# The mtime of <path> with sub-second precision, or "absent".
file_stamp() {
    local path="$1" stamp=""
    [ -e "$path" ] || { printf 'absent\n'; return 0; }
    stamp="$(stat -Lc '%y' -- "$path" 2>/dev/null || true)"
    [ -n "$stamp" ] || stamp="unknown"
    printf '%s\n' "$stamp"
}

record_user_files() {
    local path=""
    : > "$rig_root/user-files.mtimes"
    while read -r path; do
        [ -n "$path" ] || continue
        printf '%s\t%s\n' "$path" "$(file_stamp "$path")" >> "$rig_root/user-files.mtimes"
    done < <(user_files)
}

# Warn - naming the file - about every recorded user file whose mtime moved;
# returns 1 when any did. Never a refusal (the rig must still stop), but the
# caller reports it with a non-zero exit status.
check_user_files() {
    local file="$rig_root/user-files.mtimes" path="" recorded="" current="" moved=0
    [ -s "$file" ] || return 0
    while IFS=$'\t' read -r path recorded; do
        [ -n "$path" ] || continue
        current="$(file_stamp "$path")"
        [ "$current" = "$recorded" ] && continue
        warn "user-session file changed while the rig ran: $path (mtime $recorded -> $current)"
        warn "  by the rig, or by another writer in your session (your own Fooyin/app saves its config)"
        moved=1
    done < "$file"
    [ "$moved" -eq 0 ]
}

is_own_pid() {
    local pid="$1" own=""
    for own in $(own_pids); do
        [ "$own" = "$pid" ] && return 0
    done
    return 1
}

# Is this pid one this rig owns? Either recorded in <rig-root>/pids, or (for
# anything the plugin/dbus spawned) carrying a rig path in its environment.
rig_owns_pid() {
    local pid="$1" name rec_pid rec_bin rec_start
    while read -r name rec_pid rec_bin rec_start; do
        [ "$rec_pid" = "$pid" ] && return 0
    done < <(verified_recorded_pids)
    [ -n "$(pid_rig_env_entry "$pid")" ] && return 0
    return 1
}

# fooyin's single-instance socket comes from KDSingleApplication, which names
# it kdsingleapp-<user>-fooyin and resolves it through QDir::tempPath(). The rig
# launches fooyin with TMPDIR=<rig-root>/tmp, so the rig's socket lives inside
# the rig root and a fooyin started elsewhere cannot collide with it. This lists
# the fooyins that are NOT this rig's - a user's player, another rig's fooyin -
# for status, and for the start-time warning that names the residual caveat (a
# fooyin launched with TMPDIR=<rig-root>/tmp would join this rig's session).
foreign_fooyin_pids() {
    local pid argv0 raw=""
    for d in /proc/[0-9]*; do
        pid="${d#/proc/}"
        [ -r "$d/cmdline" ] || continue
        raw="$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null || true)"
        argv0="${raw%%$'\n'*}"
        case "${argv0##*/}" in
            fooyin) ;;
            *) continue ;;
        esac
        is_own_pid "$pid" && continue
        rig_owns_pid "$pid" && continue
        printf '%s\n' "$pid"
    done
}

pid_alive() {
    local pid="$1" state=""
    case "$pid" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ -r "/proc/$pid/status" ] || return 1
    state="$(sed -n 's/^State:[[:space:]]*\([A-Z]\).*/\1/p' "/proc/$pid/status" 2>/dev/null || true)"
    [ -n "$state" ] || return 1
    [ "$state" != "Z" ] || return 1
    return 0
}

# This shell and every ancestor of it: never signalled by the environ sweep,
# which would otherwise kill an interactive shell that sourced rig.env.
own_pids() {
    local pid="$$" next=""
    printf '%s\n' "$pid"
    while :; do
        next="$(sed -n 's/^PPid:[[:space:]]*//p' "/proc/$pid/status" 2>/dev/null || true)"
        [ -n "$next" ] || break
        printf '%s\n' "$next"
        [ "$next" = "1" ] && break
        [ "$next" = "0" ] && break
        pid="$next"
    done
}

# Pids whose environment proves they belong to this rig: an entry whose value
# IS <rig-root> or is a path under it (LX_RIG_ROOT=, HOME=, XDG_*=, TMPDIR=,
# LX_RIG_APP=, ...). Only this rig puts those paths into an environment, so it
# is proof of ownership - and unlike a bare LX_RIG_ROOT match it also catches a
# stray started with the rig's HOME but without the marker (a fooyin CLI
# launched from a shell that only half-adopted the rig environment).
#
# One implementation, one python pass, two modes: without a pid it sweeps /proc
# (skipping itself and its parent) and prints one "<pid>\t<KEY=VALUE>" line per
# hit; with a pid it prints just that pid's first matching entry. The callers
# use the pid and show the entry as the evidence for what they are about to
# signal.
rig_env_scan() {
    python3 - "$rig_root" "${1:-}" <<'PY' 2>/dev/null || true
import os, sys

rig = sys.argv[1]
only = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else ""
prefix = rig + "/"

def match(pid):
    try:
        with open(f"/proc/{pid}/environ", "rb") as handle:
            data = handle.read()
    except OSError:
        return None
    for item in data.split(b"\0"):
        if b"=" not in item:
            continue
        _, _, raw = item.partition(b"=")
        value = raw.decode(errors="replace")
        if value == rig or value.startswith(prefix):
            return item.decode(errors="replace")
    return None

if only:
    entry = match(only)
    if entry is not None:
        print(entry)
else:
    skip = {os.getpid(), os.getppid()}
    for entry in sorted(os.listdir("/proc")):
        if not entry.isdigit() or int(entry) in skip:
            continue
        hit = match(entry)
        if hit is not None:
            print(f"{entry}\t{hit}")
PY
}

# Pids whose environment proves they belong to this rig, as "<pid>\t<entry>".
sweep_owned_pids() {
    rig_env_scan
}

# The first environment entry of <pid> that proves rig ownership, or nothing.
pid_rig_env_entry() {
    rig_env_scan "${1:-}"
}

# The two hard exclusions, shared by the kill loop and the final leftovers
# check: never this shell or any of its ancestors, and never a shell
# interpreter (a terminal that sourced rig.env is not something the rig
# started). Prints the reason when the pid must be left alone.
skip_pid_reason() {
    local pid="$1" raw="" argv0="" base="" exe="" exebase=""
    if is_own_pid "$pid"; then
        printf 'it is this shell or one of its ancestors\n'
        return 0
    fi
    raw="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null || true)"
    argv0="${raw%%$'\n'*}"
    base="${argv0##*/}"
    base="${base#-}"          # a login shell's argv[0] is -zsh, -bash, ...
    case "$base" in
        bash|sh|dash|zsh|fish|ksh|mksh|tcsh|csh)
            printf 'a shell (%s) that sourced rig.env, not a rig process\n' "$base"
            return 0
            ;;
    esac
    # argv[0] is not always the shell's path (a re-exec, or a script's
    # interpreter), so the resolved executable is a second chance to spare it.
    exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
    exebase="${exe##*/}"
    exebase="${exebase#-}"
    case "$exebase" in
        bash|sh|dash|zsh|fish|ksh|mksh|tcsh|csh)
            printf 'a shell (%s) that sourced rig.env, not a rig process\n' "$exebase"
            return 0
            ;;
    esac
    return 1
}

wait_pids_dead() {
    local timeout="$1"
    shift
    local waited=0 alive=0 pid
    while [ "$waited" -lt "$timeout" ]; do
        alive=0
        for pid in "$@"; do
            if pid_alive "$pid"; then
                alive=1
                break
            fi
        done
        [ "$alive" -eq 0 ] && return 0
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

signal_pids() {
    local signal="$1"
    shift
    local pid
    for pid in "$@"; do
        kill "-$signal" "$pid" 2>/dev/null || true
    done
}

print_cli_help() {
    local rig_cmd="tools/gui-test/rig.sh --rig-root '$rig_root'"
    cat <<EOF

Driving this rig's Fooyin (each line already names this rig root; options must
come before the subcommand). Both subcommands carry the rig's own environment
and window, and both refuse when the rig is not running - so they can only ever
reach the rig's player, never another fooyin on the machine:

    $rig_cmd key --clearmodifiers ctrl+i      # Desktop Lyrics toggle
    $rig_cmd fooyin -p <file>                 # open + play
    $rig_cmd fooyin -u                        # pause
    $rig_cmd fooyin -t                        # play/pause
    $rig_cmd fooyin -s                        # stop
    $rig_cmd fooyin -R <ms>                   # seek backward, e.g. -R 600000
    $rig_cmd fooyin -F <ms>                   # seek forward,  e.g. -F 150000

The seek value is MILLISECONDS, not a duration string: fooyin parses it with
std::stoull, which stops at the first non-digit, so "-R 150s" seeks 150 ms.

The ALSA|null output is unthrottled (playback runs well ahead of realtime), so
never assume realtime playback. Deterministic positions come from pause-
stepping - pause, clamp to the start with an oversized backward seek, then
seek forward to the wanted millisecond:

    $rig_cmd fooyin -u
    $rig_cmd fooyin -R 600000
    $rig_cmd fooyin -F 150000

Lower level, if you want the raw CLI in your own shell: sourcing rig.env
exports the rig's HOME, XDG_* directories, TMPDIR, DISPLAY and bus address for
you (and for everything you start from that shell). TMPDIR is what puts
fooyin's single-instance socket - kdsingleapp-<user>-fooyin, resolved through
QDir::tempPath() - under $rig_root/tmp, so a bare 'fooyin -p' without that
environment cannot reach the rig's instance, and the rig's instance cannot be
reached by, or reach, the user's own player.
EOF
}

usage() {
    cat <<EOF
usage: tools/gui-test/rig.sh start|stop|status|restart [options]
       tools/gui-test/rig.sh fooyin <fooyin args...>
       tools/gui-test/rig.sh key <xdotool key args...>

  start     seed the rig root, start Xvfb + a private dbus-daemon + fooyin on
            the private display, wait for the LX Lyrics GuiPlugin to initialise
            and print the display, the pids and the follow-up commands. Refuses
            when 'fooyin --version' is not the 0.12.x this rig encodes (unless
            --force), and when the rig root already contains another rig root
  stop      terminate the processes this rig started (each recorded pid only
            while /proc/<pid>/exe and the recorded start time still match) plus,
            when the rig root carries its .rig-root marker, every process whose
            *launch* environment carries a path inside the rig root. Keeps the
            rig root with its logs. Exits 0 even when nothing was running, and
            3 when rig-owned processes survived or a user-session file changed
  status    report whether the rig is running, where its logs are, and how to
            drive the fooyin CLI; exits 0 when stopped ("not running"), 3 when
            a user-session file changed while the rig ran
  restart   stop, then start
  fooyin    run the fooyin CLI against *this* rig's instance - the rig's
            environment (HOME, XDG_*, TMPDIR, DISPLAY, private bus) is applied
            first, so the command can only reach the rig's player. Exits with
            fooyin's status; refuses when the rig is not running. Everything
            after 'fooyin' is passed through:
              rig.sh fooyin -p /tmp/lx-lyrics-gui-rig/fixtures/track.flac
  key       focus the rig's fooyin main window and send an xdotool key chord -
            the Desktop Lyrics toggle without hand-managing DISPLAY or window
            ids. Refuses when the rig is not running. Everything after 'key' is
            passed to 'xdotool key':
              rig.sh key --clearmodifiers ctrl+i

  Options must come before the subcommand: rig.sh --rig-root DIR fooyin -p F.

  --rig-root DIR   rig root; must be an absolute path (default:
                   \${LX_RIG_ROOT:-/tmp/lx-lyrics-gui-rig}), also exported as
                   LX_RIG_ROOT to every child
  --app PATH       lx-lyrics-app binary (default: lyrics-app/build/lx-lyrics-app
                   if it exists, else \$HOME/.local/bin/lx-lyrics-app, else the
                   path a previous seed recorded in <rig-root>/app-path)
  --track PATH     fixture track (default: <rig-root>/fixtures/track.flac,
                   generated by seed-config.py when missing)
  --force          start even when 'fooyin --version' is not the 0.12.x this rig
                   was built against. The rig encodes 0.12.x-specific config
                   keys ([LxLyrics], [Engine] AudioOutput, the shortcut), so an
                   unknown version may need different ones than the seeded file
  --purge          with stop/restart: remove the rig root (logs and all). Only
                   when the rig root proves itself: it carries the .rig-root
                   marker this revision writes, or the rig.env of an earlier
                   revision, and it contains no other rig root. Refused while
                   rig-owned processes survived teardown (exit 3, the root and
                   its logs are kept); a user-session file tripwire hit only
                   warns - the run still exits 3, but the purge goes ahead when
                   it was asked for
  --help           show this help and exit

  Exit status: 0 success (including a stop that found nothing to do), 1 a
  refusal or an error, 3 a teardown verification failure (a rig-owned process
  survived, or the rig wrote a file outside its root - see stop/status).

  Environment:
    LX_RIG_ROOT           the rig root (default /tmp/lx-lyrics-gui-rig)
    LX_RIG_DISPLAY_SIZE   Xvfb screen size (default 1440x900x24)
    LX_RIG_START_TIMEOUT  seconds to wait for 'GuiPlugin initialised'
                          (default 45)

\$RIG is the rig root. Config seeding is idempotent and happens on every start
that actually builds the rig (an already-alive rig warns, prints status and
returns without seeding - use restart to re-seed). A rig root is marked by a
.rig-root file: the environment sweep and --purge run only for a marked root, so
'stop' on some other directory can never signal anything.
EOF
}

# --- Start -------------------------------------------------------------------

resolve_app() {
    local candidate=""
    if [ -n "$app_flag" ]; then
        printf '%s\n' "$app_flag"
        return 0
    fi
    candidate="$repo_root/lyrics-app/build/lx-lyrics-app"
    if [ -x "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi
    if [ -n "${HOME:-}" ]; then
        candidate="$HOME/.local/bin/lx-lyrics-app"
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi
    candidate="$rig_root/app-path"
    if [ -s "$candidate" ]; then
        candidate="$(head -n 1 "$candidate" 2>/dev/null || true)"
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi
    return 0
}

write_rig_env() {
    # Every entry is exported: `source rig.env` must put the rig's environment
    # into the *children* a source-ing shell starts (TMPDIR pins the CLI to the
    # rig's fooyin), not merely set shell-local variables for entries the
    # ambient environment does not already export (TMPDIR usually is not).
    local file="$1"
    shift
    {
        printf '# Written by tools/gui-test/rig.sh - source this to drive the rig.\n'
        printf '# Every entry is exported, so children inherit it.\n'
        while [ $# -gt 1 ]; do
            printf 'export %s=%s\n' "$1" "$(quote "$2")"
            shift 2
        done
    } > "$file"
}

any_recorded_alive() {
    local name pid binary starttime
    while read -r name pid binary starttime; do
        return 0
    done < <(verified_recorded_pids)
    return 1
}

cmd_start() {
    local app track
    app="$(resolve_app)"
    if [ -z "$app" ]; then
        die "no lx-lyrics-app binary found (build lyrics-app, or pass --app PATH)"
    fi
    [ -x "$app" ] || die "the lyrics-app binary is not executable: $app"
    track="${track_flag:-$rig_root/fixtures/track.flac}"

    if any_recorded_alive; then
        warn "the rig at $rig_root already looks alive; use 'restart' to rebuild it"
        cmd_status
        return 0
    fi

    local foreign=()
    while read -r pid; do
        [ -n "$pid" ] && foreign+=("$pid")
    done < <(foreign_fooyin_pids)
    if [ "${#foreign[@]}" -gt 0 ]; then
        warn "another fooyin is already running: ${foreign[*]}"
        warn "starting anyway - the rig gives its own fooyin TMPDIR=$rig_root/tmp, so"
        warn "fooyin's single-instance socket (kdsingleapp-<user>-fooyin, resolved"
        warn "through QDir::tempPath()) lands inside the rig root and cannot collide"
        warn "with that player, and this rig's 'fooyin -p/-u/...' commands cannot"
        warn "reach it. A fooyin launched with TMPDIR=$rig_root/tmp would join this"
        warn "rig's session instead - use only the commands rig.sh prints."
    fi

    [ -f "$appwrap_script" ] || die "the AppPath wrapper is missing: $appwrap_script"

    command -v fooyin >/dev/null 2>&1 || die "fooyin is not on PATH"
    command -v dbus-daemon >/dev/null 2>&1 || die "dbus-daemon is not on PATH"
    command -v python3 >/dev/null 2>&1 || die "python3 is not on PATH"
    command -v Xvfb >/dev/null 2>&1 || die "Xvfb is not on PATH (the rig needs its own X display)"
    for tool in xdotool xwininfo; do
        command -v "$tool" >/dev/null 2>&1 \
            || warn "$tool is not on PATH: 'rig.sh key', the window listing in status and the printed xdotool hints need it"
    done

    local nested=""
    nested="$(root_contains_nested_rig "$rig_root")"
    if [ -n "$nested" ]; then
        die "refusing to start at $rig_root: it contains another rig root ($nested)"
    fi
    # N5: never take over a directory that already holds something else - start
    # would write a marker into it and a later `stop --purge` would delete it.
    require_takeover_ok

    mkdir -p "$rig_root"
    # NEW-A: the takeover check has just passed, so this directory is ours from
    # this moment - mark it straight away. An abort after the scaffold but before
    # the seed (a missing --track, the version-probe timeout) must not leave a
    # directory that start/restart/stop --purge all refuse to take back.
    {
        printf '# This directory is a tools/gui-test/rig.sh rig root.\n'
        printf '# Its presence is what licenses the environment sweep and --purge.\n'
        printf 'root %s\n' "$rig_root"
        printf 'started %s\n' "$(date -Is 2>/dev/null || date)"
    } > "$rig_root/.rig-root"
    local home="$rig_root/home"
    mkdir -p "$home/.config" "$home/.local/share" "$home/.local/state" "$home/.cache" \
             "$rig_root/run" "$rig_root/tmp" "$rig_root/fixtures"
    chmod 700 "$rig_root/run" "$rig_root/tmp"
    # F10: the plugin execs its AppPath, so the wrapper must be executable - and
    # the checkout must not be written to. Copy it into the rig root on every
    # start and point [LxLyrics] AppPath at the copy (seed-config.py).
    cp -f -- "$appwrap_script" "$rig_root/appwrap.py"
    chmod 755 "$rig_root/appwrap.py"

    # F15 (plan section 7): the rig encodes 0.12.x-specific config keys, so
    # refuse a different player before anything is seeded or started. TMPDIR
    # already points into the rig, so even this probe's single-instance socket
    # lands inside the rig root. --force is the documented escape hatch.
    local fooyin_version="" fooyin_probe="" fooyin_probe_status=0
    # N8: a wedged player (or a stale single-instance socket) must not hang
    # start before anything is diagnosed, so the probe is bounded.
    fooyin_probe="$(env -u LX_RIG_ROOT -u WAYLAND_DISPLAY -u WAYLAND_SOCKET \
        HOME="$home" XDG_RUNTIME_DIR="$rig_root/run" TMPDIR="$rig_root/tmp" \
        XDG_CONFIG_HOME="$home/.config" QT_QPA_PLATFORM=xcb \
        timeout 10 fooyin --version 2>/dev/null)" || fooyin_probe_status=$?
    if [ "$fooyin_probe_status" -eq 124 ]; then
        die "'fooyin --version' did not answer within 10s (a wedged fooyin, or a stale single-instance socket?)"
    fi
    fooyin_version="$(printf '%s\n' "$fooyin_probe" | head -n 1)"
    [ -n "$fooyin_version" ] || fooyin_version="fooyin (unknown version)"
    local fooyin_number=""
    fooyin_number="$(printf '%s\n' "$fooyin_version" | sed -n 's/^fooyin \([0-9][0-9.]*\).*/\1/p')"
    case "${fooyin_number:-}" in
        0.12|0.12.*) ;;
        *)
            if [ "$force" -eq 1 ]; then
                warn "unexpected fooyin version '$fooyin_version' (this rig encodes 0.12.x config keys); continuing because --force was given"
            else
                die "unsupported fooyin version: '$fooyin_version' (this rig encodes 0.12.x config keys; pass --force to start anyway)"
            fi
            ;;
    esac

    # N3: baseline the operator's files *before* seeding, so a leak during
    # seeding cannot become the baseline and stay invisible for ever.
    record_user_files

    # NEW-2: re-verify the takeover immediately before the first seed write - the
    # window since the first check is small, but a writer that filled the
    # directory in it would otherwise be taken over.
    require_takeover_ok --scaffold

    if [ -n "$track_flag" ]; then
        python3 "$seed_script" --rig-root "$rig_root" --app "$app" --track "$track"
    else
        python3 "$seed_script" --rig-root "$rig_root" --app "$app"
    fi

    : > "$rig_root/fooyin.log"
    : > "$rig_root/feed.log"
    : > "$rig_root/xvfb.log"
    : > "$rig_root/dbus.log"
    rm -f "$rig_root/pids" "$rig_root/display" "$rig_root/rig.env" "$rig_root/display.tmp"

    step "starting Xvfb"
    # -displayfd hands Xvfb a writable fd: it picks a free display number and
    # writes it there, so two rigs never collide on :99. -noreset keeps the
    # display alive across a fooyin restart; -ac skips X authority entirely.
    exec 7>"$rig_root/display.tmp"
    Xvfb -displayfd 7 -screen 0 "$display_size" -nolisten tcp -ac -noreset \
        >>"$rig_root/xvfb.log" 2>&1 &
    local xvfb_pid=$!
    exec 7>&-

    local display_number="" waited=0
    while [ "$waited" -lt 300 ]; do
        display_number="$(tr -d '[:space:]' < "$rig_root/display.tmp" 2>/dev/null || true)"
        [ -n "$display_number" ] && break
        pid_alive "$xvfb_pid" || break
        sleep 0.1
        waited=$((waited + 1))
    done
    if [ -z "$display_number" ]; then
        warn "Xvfb did not report a display number; last lines of $rig_root/xvfb.log:"
        tail -n 20 "$rig_root/xvfb.log" >&2 || true
        signal_pids TERM "$xvfb_pid"
        rm -f "$rig_root/display.tmp"
        exit 1
    fi
    printf '%s\n' "$display_number" > "$rig_root/display"
    rm -f "$rig_root/display.tmp"
    local display=":$display_number"
    note "display  $display (Xvfb pid $xvfb_pid)"

    step "starting a private dbus-daemon"
    local dbus_out="" dbus_addr="" dbus_pid=""
    dbus_out="$(dbus-daemon --session --fork --print-address=1 --print-pid=1 2>>"$rig_root/dbus.log" || true)"
    dbus_addr="$(printf '%s\n' "$dbus_out" | sed -n '1p')"
    dbus_pid="$(printf '%s\n' "$dbus_out" | sed -n '2p')"
    if [ -z "$dbus_addr" ] || [ -z "$dbus_pid" ]; then
        warn "dbus-daemon did not report an address and a pid"
        signal_pids TERM "$xvfb_pid"
        exit 1
    fi
    note "dbus     $dbus_addr (pid $dbus_pid)"

    step "starting fooyin"
    # Isolation: a private HOME + XDG tree + TMPDIR, the rig's display, the
    # rig's bus, and no Wayland left over from the user's session (a leaked
    # WAYLAND_DISPLAY is how the ad-hoc rig once killed the user's own Fooyin).
    # TMPDIR is what puts fooyin's single-instance socket inside the rig.
    env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u XDG_SESSION_TYPE \
        HOME="$home" \
        XDG_CONFIG_HOME="$home/.config" \
        XDG_DATA_HOME="$home/.local/share" \
        XDG_STATE_HOME="$home/.local/state" \
        XDG_CACHE_HOME="$home/.cache" \
        XDG_RUNTIME_DIR="$rig_root/run" \
        TMPDIR="$rig_root/tmp" \
        DISPLAY="$display" \
        DBUS_SESSION_BUS_ADDRESS="$dbus_addr" \
        QT_QPA_PLATFORM=xcb \
        LC_ALL="${LC_ALL:-C.UTF-8}" \
        LX_RIG_ROOT="$rig_root" \
        LX_RIG_APP="$app" \
        fooyin >"$rig_root/fooyin.log" 2>&1 &
    local fooyin_pid=$!
    note "fooyin   pid $fooyin_pid ($fooyin_version)"

    write_rig_env "$rig_root/rig.env" \
        HOME "$home" \
        XDG_CONFIG_HOME "$home/.config" \
        XDG_DATA_HOME "$home/.local/share" \
        XDG_STATE_HOME "$home/.local/state" \
        XDG_CACHE_HOME "$home/.cache" \
        XDG_RUNTIME_DIR "$rig_root/run" \
        TMPDIR "$rig_root/tmp" \
        DISPLAY "$display" \
        DBUS_SESSION_BUS_ADDRESS "$dbus_addr" \
        LX_RIG_ROOT "$rig_root" \
        LX_RIG_APP "$app" \
        LX_RIG_TRACK "$track" \
        FOOYIN_VERSION "$fooyin_version"
    {
        printf 'fooyin %s %s %s\n' "$fooyin_pid" "$(command -v fooyin)" \
               "$(proc_starttime "$fooyin_pid" || true)"
        printf 'dbus %s %s %s\n' "$dbus_pid" "$(command -v dbus-daemon)" \
               "$(proc_starttime "$dbus_pid" || true)"
        printf 'xvfb %s %s %s\n' "$xvfb_pid" "$(command -v Xvfb)" \
               "$(proc_starttime "$xvfb_pid" || true)"
    } > "$rig_root/pids"

    step "waiting for the LX Lyrics GuiPlugin (timeout ${start_timeout}s)"
    waited=0
    while [ "$waited" -lt $((start_timeout * 10)) ]; do
        if grep -q 'GuiPlugin initialised' "$rig_root/fooyin.log" 2>/dev/null; then
            break
        fi
        pid_alive "$fooyin_pid" || break
        sleep 0.1
        waited=$((waited + 1))
    done
    if ! grep -q 'GuiPlugin initialised' "$rig_root/fooyin.log" 2>/dev/null; then
        warn "fooyin did not log 'GuiPlugin initialised' - upstream change, or the plugin failed to load"
        printf -- '--- tail of %s ---\n' "$rig_root/fooyin.log" >&2
        tail -n 40 "$rig_root/fooyin.log" >&2 || true
        teardown >&2 || true
        exit 1
    fi
    note "plugin   GuiPlugin initialised"

    printf '\n'
    printf 'rig root  %s\n' "$rig_root"
    printf 'display   %s\n' "$display"
    printf 'pids      fooyin %s, dbus %s, xvfb %s\n' "$fooyin_pid" "$dbus_pid" "$xvfb_pid"
    printf 'app       %s\n' "$app"
    printf 'track     %s\n' "$track"
    printf 'logs      %s\n' "$rig_root/fooyin.log $rig_root/feed.log $rig_root/xvfb.log $rig_root/dbus.log"
    printf 'tmp       %s (TMPDIR: where the single-instance socket lives)\n' "$rig_root/tmp"
    printf 'env       source %s\n' "$rig_root/rig.env"
    local rig_cmd="tools/gui-test/rig.sh --rig-root '$rig_root'"
    cat <<EOF

Follow-up commands (every rig.sh line names this rig root; the raw commands
below - tail, pgrep and the lower-level block - use \$rig_root-relative paths):

    bash $rig_cmd key --clearmodifiers ctrl+i     # desktop lyrics on/off
    bash $rig_cmd fooyin -p $track
    bash $rig_cmd fooyin -u
    bash $rig_cmd fooyin -R 600000
    bash $rig_cmd fooyin -F 150000                # paused at 150 s
    pgrep -af 'lx-lyrics-app --player-feed'
    tail -f $rig_root/feed.log
    bash $rig_cmd status
    bash $rig_cmd stop

Lower level (raw CLI in your own shell):

    source $rig_root/rig.env
    DISPLAY=$display xwininfo -root -tree | grep fooyin
    win=\$(DISPLAY=$display xdotool search --name fooyin | head -n 1)
    DISPLAY=$display xdotool windowfocus "\$win"
    DISPLAY=$display xdotool key --clearmodifiers ctrl+i
    fooyin -p $track
    appwin=\$(DISPLAY=$display xdotool search --classname lx-lyrics-app | head -n 1)
    DISPLAY=$display import -window "\$appwin" /tmp/lx-lyrics-app.png
EOF
    print_cli_help
}

# --- Stop --------------------------------------------------------------------

teardown() {
    local -a recorded=() swept=() targets=()
    local name pid binary starttime verdict entry any=0
    local matched="" other="" reason="" sweep_enabled=0
    # F1.2: the environment sweep runs only for a root that proves it is a rig
    # root. Without the marker the rig cannot tell a rig-owned process from one
    # that merely happens to carry this path, so it finds nothing; recorded pids
    # are still validated and used.
    if is_rig_root "$rig_root"; then
        sweep_enabled=1
    fi

    # 1. the recorded processes, each one only while /proc/<pid> still proves it
    #    is that process (recorded start time + exe). Anything else - a recycled
    #    pid, a legacy record without a start time - is skipped and named, never
    #    signalled.
    while read -r name pid binary starttime verdict; do
        case "$verdict" in
            live)
                recorded+=("$pid|$binary|$starttime")
                ;;
            stale)
                warn "skipping recorded $name pid $pid: it is not $binary any more (recycled pid?)"
                ;;
            unverified)
                warn "skipping recorded $name pid $pid: cannot prove it is $binary (no recorded start time, or /proc/$pid/exe unreadable)"
                ;;
        esac
    done < <(recorded_pid_records)

    # 2. everything else whose environment carries a rig path: that is the proof
    #    of ownership (the plugin's child app, an appwrap, an activation spawned
    #    by the rig's own dbus-daemon). A shell interpreter is never something
    #    the rig started, and a path under a *nested* rig root belongs to that
    #    other rig (F3), so both are skipped and named.
    if [ "$sweep_enabled" -eq 1 ]; then
        while IFS=$'\t' read -r pid matched; do
            [ -n "$pid" ] || continue
            other="$(foreign_rig_root "${matched#*=}")"
            if [ -n "$other" ]; then
                warn "not signalling pid $pid: ${matched%%=*}=${matched#*=} belongs to the nested rig root $other, not $rig_root"
                continue
            fi
            if reason="$(skip_pid_reason "$pid")"; then
                warn "not signalling pid $pid: $reason"
                continue
            fi
            swept+=("$pid")
        done < <(sweep_owned_pids)
    fi

    # N4: eligibility is re-proved immediately before the TERM, exactly as the
    # KILL loop does - the verdicts and the sweep are a moment old by now, and a
    # pid can have exited and been recycled in between.
    local -a term=() term_recorded=() term_evidence=()
    for entry in ${recorded[@]+"${recorded[@]}"}; do
        pid="${entry%%|*}"
        binary="${entry#*|}"; binary="${binary%%|*}"
        starttime="${entry##*|}"
        if ! pid_is "$pid" "$binary" "$starttime"; then
            warn "not signalling recorded pid $pid: it is no longer the recorded $binary"
            continue
        fi
        term+=("$pid")
        term_recorded+=("$pid|$binary")
    done
    for pid in ${swept[@]+"${swept[@]}"}; do
        entry="$(pid_rig_env_entry "$pid")"
        if [ -z "$entry" ]; then
            warn "not signalling swept pid $pid: it no longer proves rig ownership"
            continue
        fi
        other="$(foreign_rig_root "${entry#*=}")"
        if [ -n "$other" ]; then
            warn "not signalling swept pid $pid: it belongs to the nested rig root $other"
            continue
        fi
        if reason="$(skip_pid_reason "$pid")"; then
            warn "not signalling swept pid $pid: $reason"
            continue
        fi
        term+=("$pid")
        term_evidence+=("$pid  $entry")
    done

    if [ "${#term[@]}" -gt 0 ]; then
        if [ "${#term_recorded[@]}" -gt 0 ]; then
            printf 'stopping recorded processes:'
            for entry in "${term_recorded[@]}"; do
                pid="${entry%%|*}"
                binary="${entry#*|}"
                printf ' %s(%s)' "$pid" "${binary##*/}"
            done
            printf '\n'
        fi
        if [ "${#term_evidence[@]}" -gt 0 ]; then
            printf 'stopping %s rig-owned process(es) found by the environment sweep:\n' "${#term_evidence[@]}"
            for entry in "${term_evidence[@]}"; do
                printf '  %s\n' "$entry"
            done
        fi
        signal_pids TERM "${term[@]}"
        any=1
    fi
    targets=(${term[@]+"${term[@]}"})

    if [ "$any" -eq 1 ] && ! wait_pids_dead 50 "${targets[@]}"; then
        printf 'escalating to SIGKILL for the survivors\n'
        # F21: eligibility is re-proved immediately before each KILL - between
        # the sweep and now a pid may have exited and been recycled.
        for entry in "${recorded[@]}"; do
            pid="${entry%%|*}"
            binary="${entry#*|}"; binary="${binary%%|*}"
            starttime="${entry##*|}"
            if ! pid_is "$pid" "$binary" "$starttime"; then
                warn "not re-killing recorded pid $pid: it is no longer the recorded $binary"
                continue
            fi
            signal_pids KILL "$pid"
        done
        for pid in ${swept[@]+"${swept[@]}"}; do
            entry="$(pid_rig_env_entry "$pid")"
            if [ -z "$entry" ]; then
                warn "not re-killing swept pid $pid: it no longer proves rig ownership"
                continue
            fi
            other="$(foreign_rig_root "${entry#*=}")"
            if [ -n "$other" ]; then
                warn "not re-killing swept pid $pid: it belongs to the nested rig root $other"
                continue
            fi
            if reason="$(skip_pid_reason "$pid")"; then
                warn "not re-killing swept pid $pid: $reason"
                continue
            fi
            signal_pids KILL "$pid"
        done
        wait_pids_dead 20 "${targets[@]}" || true
    fi

    # Final word: nothing the rig should have killed may still carry a rig path
    # in its environment. The same exclusions apply, or a shell launched with rig
    # paths in its environment - an operator terminal the sweep cannot tell from
    # a rig process - would be reported as one that got away.
    local leftovers=()
    if [ "$sweep_enabled" -eq 1 ]; then
        while IFS=$'\t' read -r pid matched; do
            [ -n "$pid" ] || continue
            other="$(foreign_rig_root "${matched#*=}")"
            [ -z "$other" ] || continue
            if skip_pid_reason "$pid" >/dev/null; then
                continue
            fi
            pid_alive "$pid" || continue
            leftovers+=("$pid  ${matched:-<no matching entry>}")
        done < <(sweep_owned_pids)
    fi
    if [ "${#leftovers[@]}" -gt 0 ]; then
        warn "rig-owned processes survived teardown:"
        for entry in "${leftovers[@]}"; do
            pid="${entry%% *}"
            printf '  pid %s  %s\n' "$pid" "$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)" >&2
        done
        return 1
    fi

    if [ "$any" -eq 0 ]; then
        printf 'nothing was running\n'
    fi
    return 0
}

cmd_stop() {
    local teardown_status=0 user_status=0
    if [ ! -d "$rig_root" ]; then
        printf 'not running (no rig root at %s)\n' "$rig_root"
        return 0
    fi
    if ! is_rig_root "$rig_root" && [ ! -s "$rig_root/pids" ]; then
        # A directory that never was a rig root: a silent no-op for plain stop,
        # but an explicit --purge still gets the refusal (and the hand-run
        # rm -rf hint) instead of being swallowed by the early return.
        if [ "$purge" -eq 0 ]; then
            printf 'not running (no rig root at %s)\n' "$rig_root"
            return 0
        fi
        cmd_purge
        return $?
    fi
    if ! teardown; then
        teardown_status=1
    fi
    check_user_files || user_status=1

    if [ "$purge" -eq 1 ]; then
        if [ "$teardown_status" -ne 0 ]; then
            warn "not purging $rig_root: rig-owned processes survived teardown"
            warn "the rig root and its logs are kept; stop them and retry"
            return 3
        fi
        # N1: only survivors block the purge. A user-file change is reported and
        # makes the run exit 3, but the operator asked for removal - and the
        # warnings are already on screen - so the purge still happens.
        cmd_purge || return $?
        if [ "$user_status" -ne 0 ]; then
            return 3
        fi
        return 0
    fi
    printf 'rig root kept at %s (logs); use --purge to remove it\n' "$rig_root"
    if [ "$teardown_status" -ne 0 ] || [ "$user_status" -ne 0 ]; then
        return 3
    fi
    return 0
}

cmd_purge() {
    # --purge is an rm -rf, so the target must prove it is a rig root before
    # anything happens. The shared root validator (run for every subcommand)
    # already refused /, top-level directories, $HOME, the repository and any
    # directory containing either of them. On top of that this refuses a
    # directory that is not a rig root at all (no .rig-root marker and no
    # rig.env of a previous revision) and one that contains a nested rig root.
    local target="$rig_root" home=""
    while [ "$target" != "/" ] && [ "${target%/}" != "$target" ]; do
        target="${target%/}"
    done
    case "$target" in
        ""|"/") die "refusing to purge '$rig_root'" ;;
    esac
    if [ ! -e "$target" ]; then
        printf 'nothing to purge at %s\n' "$target"
        return 0
    fi
    if ! is_rig_root "$target"; then
        warn "refusing to purge $target: it is not a rig root (no .rig-root marker and no rig.env naming it)"
        home="$(user_home)"
        if [ -n "$home" ]; then
            case "$target/" in
                "$home"/*) warn "  it is also inside \$HOME ($home)" ;;
            esac
        fi
        case "$target/" in
            "$repo_root"/*) warn "  it is also inside the repository ($repo_root)" ;;
        esac
        printf 'if you really mean to remove it, do it by hand:\n    rm -rf -- %s\n' "$(quote "$target")" >&2
        return 1
    fi
    local nested=""
    nested="$(root_contains_nested_rig "$target")"
    if [ -n "$nested" ]; then
        die "refusing to purge $target: it contains another rig root ($nested)"
    fi
    printf 'purging %s\n' "$target"
    rm -rf -- "$target"
}

# --- Status ------------------------------------------------------------------

cmd_status() {
    local display_number="" display="" name pid binary starttime verdict any=0 user_status=0
    if [ ! -d "$rig_root" ]; then
        printf 'not running (no rig root at %s)\n' "$rig_root"
        print_cli_help
        return 0
    fi
    [ -s "$rig_root/display" ] && display_number="$(head -n 1 "$rig_root/display" 2>/dev/null || true)"
    [ -n "$display_number" ] && display=":$display_number"

    while read -r name pid binary starttime verdict; do
        case "$verdict" in
            live)
                any=1
                printf '%s  pid %s  %s\n' "$name" "$pid" "$binary"
                ;;
            stale)
                printf 'recorded %s pid %s is alive but is not %s any more (skipped)\n' "$name" "$pid" "$binary"
                ;;
            unverified)
                printf 'recorded %s pid %s cannot be verified as %s, so it is never signalled (skipped)\n' "$name" "$pid" "$binary"
                ;;
        esac
    done < <(recorded_pid_records)

    # N2: the re-check runs whatever the pid table says - a crashed rig can
    # still have leaked into the operator's files, and that must not read as a
    # clean "not running".
    check_user_files || user_status=1

    if [ "$any" -eq 0 ]; then
        printf 'not running (rig root %s keeps the logs of the last run)\n' "$rig_root"
        print_cli_help
        if [ "$user_status" -ne 0 ]; then
            return 3
        fi
        return 0
    fi

    printf '\nrig running\n'
    printf '  rig root  %s\n' "$rig_root"
    printf '  display   %s\n' "${display:-<unknown>}"
    printf '  logs      %s/fooyin.log %s/feed.log %s/xvfb.log %s/dbus.log\n' \
           "$rig_root" "$rig_root" "$rig_root" "$rig_root"
    printf '  env       source %s/rig.env\n' "$rig_root"
    if grep -q 'GuiPlugin initialised' "$rig_root/fooyin.log" 2>/dev/null; then
        printf '  plugin    GuiPlugin initialised\n'
    else
        printf '  plugin    no "GuiPlugin initialised" line in fooyin.log\n'
    fi
    if ! rig_running; then
        printf "  warning   the recorded fooyin is gone (stale rig root); run 'rig.sh restart'\n"
    fi
    local status_foreign=()
    while read -r pid; do
        [ -n "$pid" ] && status_foreign+=("$pid")
    done < <(foreign_fooyin_pids)
    if [ "${#status_foreign[@]}" -gt 0 ]; then
        printf '  warning   another fooyin is running: %s (its socket is elsewhere; not touched)\n' \
               "${status_foreign[*]}"
    fi
    local sock=""
    for sock in "$rig_root/tmp"/kdsingleapp-*; do
        [ -e "$sock" ] || continue
        printf '  socket    %s\n' "$sock"
        break
    done
    if [ -n "$display" ] && command -v xdotool >/dev/null 2>&1; then
        local main_window="" app_window=""
        main_window="$(first_window_of_class "$display" fooyin)"
        if [ -n "$main_window" ]; then
            printf '  main win  0x%x %s\n' "$main_window" \
                   "$(DISPLAY="$display" xdotool getwindowgeometry "$main_window" 2>/dev/null | sed -n 's/^ *Geometry: /Geometry: /p')"
        else
            printf '  main win  none found on %s\n' "$display"
        fi
        app_window="$(first_window_of_class "$display" lx-lyrics-app)"
        if [ -n "$app_window" ]; then
            printf '  app win   0x%x %s\n' "$app_window" \
                   "$(DISPLAY="$display" xdotool getwindowgeometry "$app_window" 2>/dev/null | sed -n 's/^ *Geometry: /Geometry: /p')"
        else
            printf '  app win   no lx-lyrics-app window (desktop lyrics off?)\n'
        fi
    fi
    if [ "$user_status" -ne 0 ]; then
        printf '  warning   a user-session file changed while the rig ran (see the warnings above)\n'
    fi
    print_cli_help
    if [ "$user_status" -ne 0 ]; then
        return 3
    fi
    return 0
}

cmd_restart() {
    cmd_stop
    cmd_start
}

# --- Drive the rig (fooyin / key) ---------------------------------------------

# Is the rig's own player up? The recorded fooyin pid must be alive and still
# name the fooyin binary.
rig_running() {
    local name pid binary starttime
    while read -r name pid binary starttime; do
        [ "$name" = "fooyin" ] || continue
        return 0
    done < <(verified_recorded_pids)
    return 1
}

require_running() {
    local what="$1"
    if rig_running; then
        return 0
    fi
    die "'$what' needs a live rig: $rig_root is not running (start it with: tools/gui-test/rig.sh start)"
}

# The first window xdotool reports for <classname> whose geometry is wider than
# one pixel - Qt keeps 1x1 helper windows around that must not be mistaken for
# the real window. Prints the window id, or nothing when there is none (the
# callers test for empty, so this always exits 0).
first_window_of_class() {
    local display="$1" classname="$2" wid="" geom="" width=""
    while read -r wid; do
        [ -n "$wid" ] || continue
        geom="$(DISPLAY="$display" xdotool getwindowgeometry --shell "$wid" 2>/dev/null || true)"
        width="$(printf '%s\n' "$geom" | sed -n 's/^WIDTH=//p')"
        case "$width" in ''|*[!0-9]*) continue ;; esac
        [ "$width" -gt 1 ] || continue
        printf '%s\n' "$wid"
        return 0
    done < <(DISPLAY="$display" xdotool search --classname "$classname" 2>/dev/null || true)
    return 0
}

# The rig's display plus its main fooyin window id (skipping the 1x1 helpers Qt
# keeps around), for xdotool.
rig_display() {
    local display_number=""
    [ -s "$rig_root/display" ] && display_number="$(head -n 1 "$rig_root/display" 2>/dev/null || true)"
    [ -n "$display_number" ] || die "no display recorded in $rig_root/display (stale rig root?)"
    printf ':%s\n' "$display_number"
}

rig_main_window() {
    local display="$1" wid=""
    command -v xdotool >/dev/null 2>&1 || die "xdotool is not on PATH"
    wid="$(first_window_of_class "$display" fooyin)"
    [ -n "$wid" ] || return 1
    printf '%s\n' "$wid"
}

# Run the fooyin CLI against *this* rig's instance. The rig's environment comes
# from rig.env (one source of truth, the same file a human would source), so the
# command can only ever reach the rig's socket - never a player on the ambient
# TMPDIR - which is what removes the "forgot to source rig.env" footgun.
cmd_fooyin() {
    require_running "rig.sh fooyin"
    command -v fooyin >/dev/null 2>&1 || die "fooyin is not on PATH"
    [ -s "$rig_root/rig.env" ] || die "no $rig_root/rig.env (stale rig root?)"
    [ $# -gt 0 ] || die "usage: rig.sh fooyin <fooyin args...>  (e.g. rig.sh fooyin -p <file>)"

    # shellcheck disable=SC1090  # the path is the rig's own generated env file
    . "$rig_root/rig.env"
    unset WAYLAND_DISPLAY WAYLAND_SOCKET XDG_SESSION_TYPE
    exec fooyin "$@"
}

# Focus the rig's main fooyin window and send a key chord to it: the Desktop
# Lyrics toggle without hand-managing DISPLAY or window ids.
cmd_key() {
    require_running "rig.sh key"
    [ $# -gt 0 ] || die "usage: rig.sh key <xdotool key args...>  (e.g. rig.sh key --clearmodifiers ctrl+i)"

    local display window=""
    display="$(rig_display)"
    window="$(rig_main_window "$display")" \
        || die "no fooyin main window on $display (stale rig? try: tools/gui-test/rig.sh restart)"
    DISPLAY="$display" xdotool windowfocus "$window" || die "could not focus window $window on $display"
    DISPLAY="$display" xdotool key "$@"
}

# --- Command line -------------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --purge)
            purge=1
            shift
            ;;
        --force)
            force=1
            shift
            ;;
        --rig-root|--rig-root=*|--app|--app=*|--track|--track=*)
            flag="${1%%=*}"
            if [ "$1" = "$flag" ]; then
                [ $# -ge 2 ] || die "$flag requires an argument"
                value="$2"
                shift 2
            else
                value="${1#*=}"
                shift
            fi
            [ -n "$value" ] || die "$flag requires a non-empty argument"
            case "$flag" in
                --rig-root)
                    case "$value" in
                        /*) rig_root="$value" ;;
                        *) die "the rig root must be an absolute path (got '$value')" ;;
                    esac
                    ;;
                --app) app_flag="$value" ;;
                --track) track_flag="$value" ;;
            esac
            ;;
        start|stop|status|restart)
            [ -z "$command_name" ] || die "only one command may be given (got '$command_name' and '$1')"
            command_name="$1"
            shift
            ;;
        fooyin|key)
            # Everything after these goes to fooyin / xdotool, so rig options
            # must come first: `rig.sh --rig-root DIR fooyin -p file`. A rig
            # option here would be handed to xdotool/fooyin and silently target
            # the default root, so refuse it by name instead.
            [ -z "$command_name" ] || die "only one command may be given (got '$command_name' and '$1')"
            command_name="$1"
            shift
            for arg in "$@"; do
                case "$arg" in
                    --rig-root|--rig-root=*|--app|--app=*|--track|--track=*|--force|--force=*|--purge|--purge=*|--help|--help=*)
                        die "'$arg' is a rig.sh option, not a '${command_name}' argument: options must come before the subcommand (e.g. rig.sh --rig-root DIR ${command_name} ...)"
                        ;;
                esac
            done
            passthrough_args=("$@")
            break
            ;;
        *)
            die "unknown argument '$1' (see --help)"
            ;;
    esac
done

[ -n "$command_name" ] || { usage >&2; exit 2; }

case "$rig_root" in
    /*) ;;
    *) die "the rig root must be an absolute path (got '$rig_root')" ;;
esac
rig_root="$(abspath "$rig_root")"
# F1.1: one validator, every subcommand - it runs before anything is created,
# signalled or removed.
validate_rig_root "$rig_root"
export LX_RIG_ROOT="$rig_root"

if [ "$purge" -eq 1 ] && [ "$command_name" != "stop" ] && [ "$command_name" != "restart" ]; then
    warn "--purge only means something with stop or restart; ignoring it here"
    purge=0
fi

case "$command_name" in
    start) cmd_start ;;
    stop) cmd_stop ;;
    status) cmd_status ;;
    restart) cmd_restart ;;
    fooyin) cmd_fooyin ${passthrough_args[@]+"${passthrough_args[@]}"} ;;
    key) cmd_key ${passthrough_args[@]+"${passthrough_args[@]}"} ;;
    *) die "unknown command '$command_name'" ;;
esac
