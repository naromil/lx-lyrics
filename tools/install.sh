#!/usr/bin/env bash
#
# Build + install the LX Lyrics components for the players installed on this
# machine, then point every adapter at the installed lyrics app so no path has
# to be typed in by hand.
#
#   ./tools/install.sh [--player NAME]... [--prefix DIR] [--no-autospawn] [--help]
#
# With no --player flag the script installs every adapter whose player it finds
# on this machine; repeat --player NAME to pick adapters explicitly, or pass
# `--player all` to install all six (fooyin, deadbeef, rhythmbox, audacious,
# quodlibet, vlc) whether or not they were detected.
#
# What lands where; <config> is $XDG_CONFIG_HOME (~/.config), <data> is
# $XDG_DATA_HOME (~/.local/share), <prefix> is where the app goes (default
# $HOME/.local) and every plugin goes to its own player's plugin directory:
#
#   lyrics-app  <prefix>/bin/lx-lyrics-app                            (built here)
#   fooyin      $HOME/.local/lib/fooyin/plugins/fyplugin_lxlyrics.so  (built here)
#               fooyin.conf [LxLyrics] AppPath / RememberState         (patched)
#   deadbeef    $HOME/.local/lib/deadbeef/ddb_lxlyrics.so             (built here)
#               deadbeef config lxlyrics.app_path                     (patched)
#   rhythmbox   <data>/rhythmbox/plugins/lxlyrics/                     (copied)
#               lx-lyrics/rhythmbox.conf [lx-lyrics] app-path          (patched)
#   audacious   <audacious plugin dir>/General/lxlyrics.so            (built here)
#               audacious config [lx-lyrics] app_path                 (patched)
#   quodlibet   <config>/quodlibet/plugins/lxlyrics.py                 (copied)
#               quodlibet config [plugins] lxlyrics_app_path          (patched)
#   vlc         <vlc plugin dir>/liblxlyrics_plugin.so                (built here)
#               vlcrc [lxlyrics] lxlyrics-app-path                    (patched)
#
# --no-autospawn writes each player's "do not start the lyrics session on your
# own" state: fooyin RememberState=false, deadbeef lxlyrics.enabled=0, rhythmbox
# enabled=false, `lxlyrics` dropped from Quod Libet's active_plugins, vlc
# lxlyrics-enabled=0. Audacious has no such key — the host's Plugins page toggle
# *is* the session switch, and the plugin ships disabled.
#
# Adapters whose destination directory is root-owned (a distro Audacious, a
# system VLC plugin dir) are not installed by this script: the exact `sudo
# install` command is printed and the script exits non-zero, with the other
# requested adapters still installed.
#
# Re-running is safe: cmake rebuilds incrementally and every config key is
# updated in place.
set -euo pipefail

# --- Configuration -----------------------------------------------------------

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_PROJECT="lyrics-app"
APP_DIR="$REPO_ROOT/$APP_PROJECT"
APP_BINARY_NAME="lx-lyrics-app"
BUILD_TYPE="Release"

ALL_PLAYERS=(fooyin deadbeef rhythmbox audacious quodlibet vlc)

# Install locations. The app goes to <prefix>/bin; a plugin goes to its own
# player's plugin directory, which is where that host actually looks: Fooyin
# scans <home>/.local/lib/fooyin/plugins for user plugins (corepaths.cpp:
# userPluginsPath()), NOT $XDG_DATA_HOME; DeaDBeeF scans ~/.local/lib64/deadbeef
# then ~/.local/lib/deadbeef; Rhythmbox and Quod Libet scan directories under
# <data> and <config> respectively; Audacious and VLC have no per-user plugin
# directory at all, so theirs is only ever detected or passed in.
prefix="$HOME/.local"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
fooyin_plugin_dir="$HOME/.local/lib/fooyin/plugins"
deadbeef_plugin_dir="${XDG_LOCAL_HOME:-$HOME/.local}/lib/deadbeef"
audacious_plugin_dir=""
vlc_plugin_dir=""

# Header locations: the cmake projects find installed headers on their own, so
# these only cover a header set cmake does not look at.
deadbeef_include_dir=""
audacious_include_dir=""
vlc_include_dir=""

want_remember_state=1
declare -A player_wanted=()
player_auto=0

# --- State -------------------------------------------------------------------

problems=()     # "<player>: <reason>" — an adapter could not be installed
pending_root=() # "install -m 755 <src> <dest>" — needs a privileged step
summary=()      # what was installed, printed at the end
next_steps=()   # what to do in each player after the install
copy_error=""   # copy_file() failure reason
copy_pending=0  # copy_file() deferred the copy into $pending_root
build_error=""  # build_project() failure reason

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

usage() {
    cat <<'EOF'
usage: ./tools/install.sh [--player NAME]... [--prefix DIR] [--no-autospawn] [--help]

  Without --player, every adapter whose player is installed on this machine is
  installed, together with the lyrics app all of them need.

  --player NAME   adapter to install: fooyin, deadbeef, rhythmbox, audacious,
                  quodlibet, vlc, or all (all six, detected or not). Repeat the
                  flag for several adapters.
  --prefix DIR    install the app under DIR/bin (default: $HOME/.local); each
                  adapter's plugin still goes to that player's plugin directory
  --no-autospawn  write each player's "do not start the lyrics session on your
                  own" state (see the header of this script); players without
                  such a state are reported and left alone
  --deadbeef-include DIR
  --audacious-include DIR
  --vlc-include DIR
                  header directory for an adapter whose headers are not
                  installed where cmake/pkg-config looks
  --fooyin-plugin-dir DIR
  --audacious-plugin-dir DIR
  --vlc-plugin-dir DIR
                  destination directory for that adapter's module. Defaults to
                  the player's own plugin directory: fooyin
                  $HOME/.local/lib/fooyin/plugins, audacious and vlc detected
  --help          show this help and exit

The lyrics app is always installed, since every adapter needs it.
EOF
}

# player_present PLAYER - is this player installed here? The binary on PATH is
# the signal; a config directory counts too, since a player can be installed
# without a launcher on PATH. Only used to pick the default adapter set.
player_present() {
    local player="$1"
    command -v "$player" >/dev/null 2>&1 && return 0
    case "$player" in
        fooyin) [ -d "$config_home/fooyin" ] ;;
        deadbeef) [ -d "$config_home/deadbeef" ] || [ -d "$deadbeef_plugin_dir" ] ;;
        rhythmbox) [ -d "$data_home/rhythmbox" ] ;;
        audacious) [ -d "$config_home/audacious" ] ;;
        quodlibet) [ -d "$config_home/quodlibet" ] ;;
        vlc) [ -d "$config_home/vlc" ] ;;
        *) return 1 ;;
    esac
}

require_command() {
    local command_name="$1"
    command -v "$command_name" >/dev/null 2>&1 \
        || die "required tool '$command_name' was not found on PATH (install it and re-run)"
}

# Record a per-player problem; that adapter is skipped but the other requested
# ones still get installed, and the script exits non-zero at the end.
player_failed() {
    printf 'error: %s: %s\n' "$1" "$2" >&2
    problems+=("$1: $2")
}

summary_add() {
    summary+=("$(printf '  %-10s %s' "$1" "$2")")
}

# Like summary_add, but marks a path whose install was deferred to root.
summary_add_path() {
    if [ "$copy_pending" -eq 1 ]; then
        summary_add "$1" "$2 (needs root; see below)"
    else
        summary_add "$1" "$2"
    fi
}

next_steps_add() {
    next_steps+=("$(printf '  %-10s %s' "$1" "$2")")
}

# A running player re-reads and rewrites its own config, which can drop the
# changes made here; the message is the difference between "it worked" and
# "it worked after a restart".
warn_if_running() {
    local player="$1"
    pgrep -x "$player" >/dev/null 2>&1 || return 0
    printf 'warning: %s\n' "$player is running: restart it after the install" >&2
    printf '         the running instance may write %s back on exit\n' "$2" >&2
}

# --- Command line -------------------------------------------------------------

add_player() {
    local name="$1"
    case "$name" in
        all)
            for name in "${ALL_PLAYERS[@]}"; do player_wanted[$name]=1; done
            ;;
        fooyin|deadbeef|rhythmbox|audacious|quodlibet|vlc)
            player_wanted[$name]=1
            ;;
        *)
            die "unknown player '$name' (see --help)"
            ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --no-autospawn)
            want_remember_state=0
            shift
            ;;
        --player|--player=*|--prefix|--prefix=*|--deadbeef-include|--deadbeef-include=*|\
        --audacious-include|--audacious-include=*|--vlc-include|--vlc-include=*|\
        --fooyin-plugin-dir|--fooyin-plugin-dir=*|\
        --audacious-plugin-dir|--audacious-plugin-dir=*|--vlc-plugin-dir|--vlc-plugin-dir=*)
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
                --player) add_player "$value" ;;
                --prefix) prefix="$value" ;;
                --deadbeef-include) deadbeef_include_dir="$value" ;;
                --audacious-include) audacious_include_dir="$value" ;;
                --vlc-include) vlc_include_dir="$value" ;;
                --fooyin-plugin-dir) fooyin_plugin_dir="$value" ;;
                --audacious-plugin-dir) audacious_plugin_dir="$value" ;;
                --vlc-plugin-dir) vlc_plugin_dir="$value" ;;
            esac
            ;;
        *)
            die "unknown argument '$1' (see --help)"
            ;;
    esac
done

if [ "${#player_wanted[@]}" -eq 0 ]; then
    # No adapter named: install the ones this machine actually has.
    player_auto=1
    detected=()
    undetected=()
    for player in "${ALL_PLAYERS[@]}"; do
        if player_present "$player"; then
            player_wanted[$player]=1
            detected+=("$player")
        else
            undetected+=("$player")
        fi
    done
fi

# --- Resolve install locations -------------------------------------------------

bin_dir="$prefix/bin"
app_path="$bin_dir/$APP_BINARY_NAME"

remember_value=false
if [ "$want_remember_state" -eq 1 ]; then
    remember_value=true
fi

# --- Pre-flight checks ---------------------------------------------------------

require_command cmake
require_command ninja
require_command awk

[ -d "$APP_DIR" ] || die "lyrics-app source directory not found: $APP_DIR"

if [ "$player_auto" -eq 1 ]; then
    if [ "${#detected[@]}" -eq 0 ]; then
        step "No player detected: installing the lyrics app only"
        note "pass --player NAME (or --player all) to install an adapter anyway"
    else
        step "Detected players: ${detected[*]}"
        if [ "${#undetected[@]}" -gt 0 ]; then
            note "not installed here: ${undetected[*]} (--player NAME installs one anyway)"
        fi
    fi
fi

# --- Build + install helpers -----------------------------------------------------

# configure_failure_reason PROJECT OUTPUT - what the summary says when a
# configure fails: a known missing prerequisite names what fixes it, and the
# cmake error itself was already printed above.
configure_failure_reason() {
    local project="$1" output="$2"
    case "$project" in
        fooyin)
            case "$output" in
                *FindFooyin.cmake*|*'package configuration file provided by "Fooyin"'*)
                    printf 'cmake did not find Fooyin (install Fooyin built with INSTALL_HEADERS=ON)'
                    return 0
                    ;;
            esac
            ;;
        deadbeef)
            case "$output" in
                *DEADBEEF_INCLUDE_DIR*)
                    printf 'DeaDBeeF headers were not found (pass --deadbeef-include DIR)'
                    return 0
                    ;;
            esac
            ;;
        audacious)
            case "$output" in
                *AUDACIOUS_INCLUDE_DIR*)
                    printf 'Audacious headers were not found (install the audacious dev package or pass --audacious-include DIR)'
                    return 0
                    ;;
            esac
            ;;
        vlc)
            case "$output" in
                *VLC_INCLUDE_DIR*)
                    printf 'VLC module headers were not found (install the vlc dev package or pass --vlc-include DIR)'
                    return 0
                    ;;
            esac
            ;;
    esac
    printf 'cmake configure failed (see the error above)'
}

# build_project PROJECT DIR ARTIFACT [cmake args...]
# Builds one project in Release and verifies its artifact appeared.
# 0 = built, 1 = failed with the reason in $build_error.
build_project() {
    local project="$1" dir="$2" artifact="$3"
    shift 3

    build_error=""
    if [ ! -d "$dir" ]; then
        build_error="source directory not found: $dir"
        return 1
    fi

    step "Building $project ($BUILD_TYPE)"
    # Configure separately from the build: a missing dependency shows up there,
    # and its output is what lets the summary name the missing piece.
    local configure_output="" configure_status=0
    configure_output="$(
        cd "$dir" && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@" 2>&1
    )" || configure_status=$?
    if [ "$configure_status" -ne 0 ]; then
        printf '%s\n' "$configure_output" >&2
        build_error="$(configure_failure_reason "$project" "$configure_output")"
        return 1
    fi
    if ! (
        cd "$dir"
        cmake --build build
    ); then
        build_error="the build of $project failed (see the output above)"
        return 1
    fi

    # A concurrent LSP/indexer may reconfigure the build dir and transiently
    # remove outputs; rebuild incrementally before giving up.
    if [ ! -e "$dir/build/$artifact" ]; then
        step "Artifact missing after build; rebuilding $project"
        (
            cd "$dir"
            cmake --build build
        ) || {
            build_error="the rebuild of $project failed (see the output above)"
            return 1
        }
    fi
    if [ ! -e "$dir/build/$artifact" ]; then
        build_error="the build of $project did not produce $artifact"
        return 1
    fi
    return 0
}

# build_adapter PLAYER PROJECT DIR ARTIFACT [cmake args...] — build_adapter is
# build_project with the failure recorded as that player's problem.
build_adapter() {
    local player="$1"
    shift
    if ! build_project "$@"; then
        player_failed "$player" "$build_error"
        return 1
    fi
    return 0
}

# copy_file NAME SRC DEST MODE
# 0 = nothing left to do (installed, or the privileged command was recorded in
# $pending_root for the summary), 1 = failed with the reason in $copy_error.
copy_file() {
    local name="$1" src="$2" dest="$3" mode="$4"
    local dir
    dir="$(dirname "$dest")"

    step "Installing $name -> $dest"
    copy_error=""
    copy_pending=0

    if [ ! -e "$src" ]; then
        copy_error="the file to install is missing: $src"
        return 1
    fi
    if ! mkdir -p "$dir" 2>/dev/null || [ ! -w "$dir" ]; then
        warn "$name: cannot write into $dir (it needs root); run this yourself:"
        warn "    sudo install -m $mode $src $dest"
        pending_root+=("install -m $mode $src $dest")
        copy_pending=1
        return 0
    fi
    if ! cp "$src" "$dest"; then
        copy_error="cannot copy $name to $dest"
        return 1
    fi
    if ! chmod "$mode" "$dest"; then
        copy_error="cannot set the mode of $dest"
        return 1
    fi
    return 0
}

# --- Config file helpers ---------------------------------------------------------
#
# The adapters' config files are INI-style text files the players read at start
# (Fooyin, Audacious, Quod Libet, VLC, Rhythmbox) plus DeaDBeeF's flat
# `key value` config. These helpers update a single key in place and keep
# everything else byte for byte, so hand-tuned settings survive.

prepare_config_file() {
    local file="$1" dir
    dir="$(dirname "$file")"
    mkdir -p "$dir" || die "cannot create config directory: $dir"
    if [ ! -e "$file" ]; then
        : > "$file" || die "cannot create config file: $file"
    fi
    [ -w "$file" ] || die "config file is not writable: $file"
}

# Shared awk prelude for the INI helpers below. A line is a section header when
# it starts with `[` (so a value containing a bracket cannot be mistaken for
# one), and it defines a key when it is an unindented `key=value` line — either
# active or commented out as `#key=value` (VLC writes its defaults that way), or
# written with spaces around the `=` (configparser). Both functions read
# `section` and `key`, which every call passes with -v.
INI_AWK_FUNCS='
    function section_of(line,   i) {
        if (line !~ /^\[/)
            return ""
        i = index(line, "]")
        return i ? substr(line, 1, i) : ""
    }
    function key_of(line,   s, eq) {
        if (line ~ /^[ \t]/)      # an indented line continues a value
            return ""
        s = line
        sub(/^#+[ \t]*/, "", s)   # VLC comments out default-valued options
        eq = index(s, "=")
        if (!eq)
            return ""
        s = substr(s, 1, eq - 1)
        sub(/[ \t]+$/, "", s)
        return s
    }
'

# ini_set_key FILE SECTION KEY VALUE — replaces the key's value (collapsing
# repeated occurrences into one), inserting the key at the end of the section or
# appending the section itself when it is missing.
ini_set_key() {
    local file="$1" section="$2" key="$3" value="$4"
    local tmp="$file.tmp.$$"

    if [ ! -s "$file" ]; then
        # Nothing to preserve: write the section itself.
        printf '%s\n%s=%s\n' "$section" "$key" "$value" > "$file" \
            || die "failed to write $file"
        return 0
    fi

    awk -v section="$section" -v key="$key" -v value="$value" "$INI_AWK_FUNCS"'
        BEGIN { in_section = 0; section_seen = 0; wrote = 0; skip_value = 0 }
        {
            header = section_of($0)
            if (header != "") {
                if (in_section && !wrote) {
                    print key "=" value
                    wrote = 1
                }
                in_section = (header == section)
                if (in_section)
                    section_seen = 1
                skip_value = 0
                print
                next
            }
            if (skip_value) {
                if ($0 ~ /^[ \t]/)     # continuation of the key we replaced
                    next
                skip_value = 0
            }
            if (in_section && key_of($0) == key) {
                if (!wrote) {
                    print key "=" value
                    wrote = 1
                }
                skip_value = 1
                next
            }
            print
        }
        END {
            if (in_section && !wrote) {
                print key "=" value
                wrote = 1
            }
            if (!section_seen) {
                print ""
                print section
                print key "=" value
            }
        }
    ' "$file" > "$tmp" || die "failed to update $file"

    chmod --reference="$file" "$tmp" 2>/dev/null || true
    mv -f "$tmp" "$file" || die "failed to replace $file"
}

# ini_del_key FILE SECTION KEY — drops the key and its continuation lines.
ini_del_key() {
    local file="$1" section="$2" key="$3"
    local tmp="$file.tmp.$$"

    awk -v section="$section" -v key="$key" "$INI_AWK_FUNCS"'
        BEGIN { in_section = 0; skip_value = 0 }
        {
            header = section_of($0)
            if (header != "") {
                in_section = (header == section)
                skip_value = 0
                print
                next
            }
            if (skip_value) {
                if ($0 ~ /^[ \t]/)
                    next
                skip_value = 0
            }
            if (in_section && key_of($0) == key) {
                skip_value = 1
                next
            }
            print
        }
    ' "$file" > "$tmp" || die "failed to update $file"

    chmod --reference="$file" "$tmp" 2>/dev/null || true
    mv -f "$tmp" "$file" || die "failed to replace $file"
}

# ini_get_key FILE SECTION KEY — the last value in the section, or nothing.
ini_get_key() {
    local file="$1" section="$2" key="$3"

    awk -v section="$section" -v key="$key" "$INI_AWK_FUNCS"'
        BEGIN { in_section = 0; found = 0; value = "" }
        {
            header = section_of($0)
            if (header != "") {
                in_section = (header == section)
                next
            }
            if (in_section && key_of($0) == key) {
                value = substr($0, index($0, "=") + 1)
                sub(/^[ \t]+/, "", value)
                sub(/[ \t]+$/, "", value)
                found = 1
            }
        }
        END { if (found) print value }
    ' "$file"
}

# add_ini_list_token FILE SECTION KEY TOKEN SEP — appends TOKEN to a
# SEP-separated list unless the list already holds it (VLC's colon-separated
# `extraintf`).
add_ini_list_token() {
    local file="$1" section="$2" key="$3" token="$4" sep="$5"
    local current value

    current="$(ini_get_key "$file" "$section" "$key")"
    case "$sep$current$sep" in
        *"$sep$token$sep") return 0 ;;
    esac
    if [ -n "$current" ]; then
        value="$current$sep$token"
    else
        value="$token"
    fi
    ini_set_key "$file" "$section" "$key" "$value"
}

# DeaDBeeF's config is one flat `key value` line per key.
kv_set_key() {
    local file="$1" key="$2" value="$3"
    local tmp="$file.tmp.$$"

    awk -v key="$key" -v value="$value" '
        function key_of(line,   k) {
            k = line
            sub(/[ \t].*$/, "", k)
            return k
        }
        BEGIN { wrote = 0 }
        {
            if (key_of($0) == key) {
                if (!wrote) {
                    print key " " value
                    wrote = 1
                }
                next
            }
            print
        }
        END { if (!wrote) print key " " value }
    ' "$file" > "$tmp" || die "failed to update $file"

    chmod --reference="$file" "$tmp" 2>/dev/null || true
    mv -f "$tmp" "$file" || die "failed to replace $file"
}

kv_get_key() {
    local file="$1" key="$2"

    awk -v key="$key" '
        function key_of(line,   k) {
            k = line
            sub(/[ \t].*$/, "", k)
            return k
        }
        BEGIN { found = 0; value = "" }
        {
            if (key_of($0) == key) {
                value = substr($0, length(key) + 1)
                sub(/^[ \t]+/, "", value)
                found = 1
            }
        }
        END { if (found) print value }
    ' "$file"
}

# --- Config verification ---------------------------------------------------------
#
# The helpers above return silent success on a mismatch (an unsupported file
# layout, a key that moved), so every config change is read back.

verify_ini_value() {
    local file="$1" section="$2" key="$3" expected="$4" actual
    actual="$(ini_get_key "$file" "$section" "$key")"
    [ "$actual" = "$expected" ] \
        || die "$file: [$section] $key must be '$expected', found '$actual'"
}

verify_ini_absent() {
    local file="$1" section="$2" key="$3" actual
    actual="$(ini_get_key "$file" "$section" "$key")"
    [ -z "$actual" ] \
        || die "$file: [$section] $key is obsolete and must be gone, found '$actual'"
}

verify_kv_value() {
    local file="$1" key="$2" expected="$3" actual
    actual="$(kv_get_key "$file" "$key")"
    [ "$actual" = "$expected" ] \
        || die "$file: $key must be '$expected', found '$actual'"
}

# --- Adapter installers -----------------------------------------------------------
#
# Each installer builds, installs and configures one adapter. It returns
# non-zero when the adapter could not be installed (the reason is recorded by
# player_failed); the remaining requested adapters still run.

install_player_fooyin() {
    build_adapter fooyin "plugins/fooyin" "$REPO_ROOT/plugins/fooyin" "fyplugin_lxlyrics.so" || return 1
    copy_file "the Fooyin plugin" \
        "$REPO_ROOT/plugins/fooyin/build/fyplugin_lxlyrics.so" \
        "$fooyin_plugin_dir/fyplugin_lxlyrics.so" 755 || {
        player_failed fooyin "$copy_error"
        return 1
    }

    local conf="$config_home/fooyin/fooyin.conf"
    step "Configuring Fooyin: [LxLyrics] AppPath=$app_path RememberState=$remember_value"
    warn_if_running fooyin "$conf"
    prepare_config_file "$conf"
    ini_set_key "$conf" "[LxLyrics]" AppPath "$app_path"
    ini_set_key "$conf" "[LxLyrics]" RememberState "$remember_value"
    # The plugin keys its startup restore on a single [LxLyrics] block; a stale
    # AutoSpawn= line from the old plugin option must not survive the migration.
    ini_del_key "$conf" "[LxLyrics]" AutoSpawn
    verify_ini_value "$conf" "[LxLyrics]" AppPath "$app_path"
    verify_ini_value "$conf" "[LxLyrics]" RememberState "$remember_value"
    verify_ini_absent "$conf" "[LxLyrics]" AutoSpawn

    summary_add_path fooyin "$fooyin_plugin_dir/fyplugin_lxlyrics.so"
    summary_add "" "fooyin.conf [LxLyrics] AppPath / RememberState=$remember_value"
    next_steps_add fooyin "restart Fooyin, then View -> Desktop Lyrics"
    if [ "$want_remember_state" -eq 1 ]; then
        next_steps_add "" "a fresh install stays off until that first toggle; later sessions restore it"
    fi
}

install_player_deadbeef() {
    local extra=()
    [ -n "$deadbeef_include_dir" ] && extra=("-DDEADBEEF_INCLUDE_DIR=$deadbeef_include_dir")
    build_adapter deadbeef "plugins/deadbeef" "$REPO_ROOT/plugins/deadbeef" "ddb_lxlyrics.so" "${extra[@]}" || return 1
    copy_file "the DeaDBeeF plugin" \
        "$REPO_ROOT/plugins/deadbeef/build/ddb_lxlyrics.so" \
        "$deadbeef_plugin_dir/ddb_lxlyrics.so" 755 || {
        player_failed deadbeef "$copy_error"
        return 1
    }

    local conf="$config_home/deadbeef/config" state=""
    [ "$want_remember_state" -eq 0 ] && state=" lxlyrics.enabled=0"
    step "Configuring DeaDBeeF: lxlyrics.app_path=$app_path$state"
    warn_if_running deadbeef "$conf"
    prepare_config_file "$conf"
    kv_set_key "$conf" "lxlyrics.app_path" "$app_path"
    if [ "$want_remember_state" -eq 0 ]; then
        # The wanted state is restored at player start; 0 keeps the session off.
        kv_set_key "$conf" "lxlyrics.enabled" 0
    fi
    verify_kv_value "$conf" "lxlyrics.app_path" "$app_path"
    if [ "$want_remember_state" -eq 0 ]; then
        verify_kv_value "$conf" "lxlyrics.enabled" 0
    fi

    summary_add_path deadbeef "$deadbeef_plugin_dir/ddb_lxlyrics.so"
    summary_add "" "deadbeef config lxlyrics.app_path"
    next_steps_add deadbeef "restart DeaDBeeF, then View -> LX Lyrics"
}

install_player_rhythmbox() {
    local src_dir="$REPO_ROOT/plugins/rhythmbox"
    local plugin_dir="$data_home/rhythmbox/plugins/lxlyrics"
    local conf="$config_home/lx-lyrics/rhythmbox.conf"

    if ! copy_file "the Rhythmbox plugin module" "$src_dir/lxlyrics.py" "$plugin_dir/lxlyrics.py" 644; then
        player_failed rhythmbox "$copy_error"
        return 1
    fi
    if ! copy_file "the Rhythmbox plugin metadata" "$src_dir/lxlyrics.plugin" "$plugin_dir/lxlyrics.plugin" 644; then
        player_failed rhythmbox "$copy_error"
        return 1
    fi

    local state=""
    [ "$want_remember_state" -eq 0 ] && state=" enabled=false"
    step "Configuring Rhythmbox: [lx-lyrics] app-path=$app_path$state"
    warn_if_running rhythmbox "$conf"
    prepare_config_file "$conf"
    ini_set_key "$conf" "[lx-lyrics]" app-path "$app_path"
    if [ "$want_remember_state" -eq 0 ]; then
        # The session state the plugin remembers across starts.
        ini_set_key "$conf" "[lx-lyrics]" enabled false
    fi
    verify_ini_value "$conf" "[lx-lyrics]" app-path "$app_path"
    if [ "$want_remember_state" -eq 0 ]; then
        verify_ini_value "$conf" "[lx-lyrics]" enabled false
    fi

    summary_add_path rhythmbox "$plugin_dir"
    summary_add "" "rhythmbox.conf [lx-lyrics] app-path"
    next_steps_add rhythmbox "restart Rhythmbox (the plugin enables itself), then View -> LX Lyrics"
}

# Audacious scans <PluginDir>/General, where PluginDir is the compiled-in
# <prefix>/lib/audacious, relocated to match the running executable. There is no
# per-user plugin directory. pkg-config names the installed one; otherwise it is
# derived from the audacious binary.
audacious_plugin_dir_default() {
    local plugin_dir binary prefix lib

    plugin_dir="$(pkg-config --variable=plugin_dir audacious 2>/dev/null || true)"
    if [ -n "$plugin_dir" ]; then
        printf '%s/General\n' "$plugin_dir"
        return 0
    fi
    binary="$(command -v audacious 2>/dev/null || true)"
    if [ -n "$binary" ]; then
        prefix="$(dirname "$(dirname "$(readlink -f "$binary")")")"
        for lib in lib64 lib; do
            if [ -d "$prefix/$lib/audacious" ]; then
                printf '%s/%s/audacious/General\n' "$prefix" "$lib"
                return 0
            fi
        done
        printf '%s/lib/audacious/General\n' "$prefix"
        return 0
    fi
    return 1
}

install_player_audacious() {
    local extra=() dest_dir conf
    [ -n "$audacious_include_dir" ] && extra=("-DAUDACIOUS_INCLUDE_DIR=$audacious_include_dir")
    build_adapter audacious "plugins/audacious" "$REPO_ROOT/plugins/audacious" "lxlyrics.so" "${extra[@]}" || return 1

    dest_dir="$audacious_plugin_dir"
    if [ -z "$dest_dir" ]; then
        dest_dir="$(audacious_plugin_dir_default)" || {
            player_failed audacious "cannot locate the Audacious plugin directory; pass --audacious-plugin-dir DIR"
            return 1
        }
    fi
    copy_file "the Audacious plugin" \
        "$REPO_ROOT/plugins/audacious/build/lxlyrics.so" \
        "$dest_dir/lxlyrics.so" 755 || {
        player_failed audacious "$copy_error"
        return 1
    }

    conf="$config_home/audacious/config"
    step "Configuring Audacious: [lx-lyrics] app_path=$app_path"
    warn_if_running audacious "$conf"
    prepare_config_file "$conf"
    ini_set_key "$conf" "[lx-lyrics]" app_path "$app_path"
    verify_ini_value "$conf" "[lx-lyrics]" app_path "$app_path"

    if [ "$want_remember_state" -eq 0 ]; then
        note "Audacious has no remembered session state: the Plugins page enable"
        note "toggle is the session switch and the plugin ships disabled."
    fi

    summary_add_path audacious "$dest_dir/lxlyrics.so"
    summary_add "" "audacious config [lx-lyrics] app_path"
    next_steps_add audacious "restart Audacious, then Settings -> Plugins -> LX Lyrics"
}

install_player_quodlibet() {
    local plugin_dir="$config_home/quodlibet/plugins"
    local conf="$config_home/quodlibet/config"

    if ! copy_file "the Quod Libet plugin module" \
        "$REPO_ROOT/plugins/quodlibet/lxlyrics.py" "$plugin_dir/lxlyrics.py" 644; then
        player_failed quodlibet "$copy_error"
        return 1
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        player_failed quodlibet "python3 is required to edit $conf"
        return 1
    fi
    local state=""
    [ "$want_remember_state" -eq 0 ] && state=" (and off Quod Libet's active_plugins)"
    step "Configuring Quod Libet: [plugins] lxlyrics_app_path=$app_path$state"
    warn_if_running quodlibet "$conf"
    prepare_config_file "$conf"
    # Quod Libet's own config library writes its config (RawConfigParser:
    # `key = value`, multi-line values as tab-indented continuation lines), so
    # the file is edited through it instead of by hand.
    if ! python3 - "$conf" "$app_path" "$want_remember_state" <<'PY'; then
import sys

from quodlibet import config

conf_path, app_path, no_autospawn = sys.argv[1:4]

config.init(conf_path)
try:
    config.add_section("plugins")
except Exception:
    pass

# `PluginConfigMixin` derives the option from PLUGIN_ID ("lxlyrics").
config.set("plugins", "lxlyrics_app_path", app_path)

if no_autospawn == "0":
    # An enabled plugin *is* a running session in Quod Libet, so staying off
    # means staying out of the active list.
    active = [p for p in config.get("plugins", "active_plugins", "").splitlines() if p]
    kept = [p for p in active if p != "lxlyrics"]
    if kept != active:
        config.set("plugins", "active_plugins", "\n".join(kept))
        print("    removed lxlyrics from [plugins] active_plugins")

config.save()
PY
        player_failed quodlibet "could not edit $conf with Quod Libet's own config library"
        return 1
    fi
    verify_ini_value "$conf" "[plugins]" lxlyrics_app_path "$app_path"

    summary_add_path quodlibet "$plugin_dir/lxlyrics.py"
    summary_add "" "quodlibet config [plugins] lxlyrics_app_path"
    next_steps_add quodlibet "restart Quod Libet, then Music -> Plugins -> LX Lyrics"
}

# VLC scans <libdir>/vlc/plugins and every directory in $VLC_PLUGIN_PATH.
vlc_plugin_dir_default() {
    local plugin_dir
    plugin_dir="$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null || true)"
    [ -n "$plugin_dir" ] || return 1
    printf '%s\n' "$plugin_dir"
}

install_player_vlc() {
    local extra=() dest_dir system_dir conf scanned=0
    [ -n "$vlc_include_dir" ] && extra=("-DVLC_INCLUDE_DIR=$vlc_include_dir")
    build_adapter vlc "plugins/vlc" "$REPO_ROOT/plugins/vlc" "liblxlyrics_plugin.so" "${extra[@]}" || return 1

    system_dir="$(vlc_plugin_dir_default || true)"
    dest_dir="$vlc_plugin_dir"
    if [ -z "$dest_dir" ]; then
        # A writable system directory is what a plain `vlc` scans; otherwise the
        # module goes to the per-user directory, which needs VLC_PLUGIN_PATH.
        if [ -n "$system_dir" ] && [ -d "$system_dir" ] && [ -w "$system_dir" ]; then
            dest_dir="$system_dir/misc"
        else
            dest_dir="$HOME/.local/lib/vlc/plugins"
        fi
    fi
    if [ -n "$system_dir" ]; then
        case "$dest_dir" in
            "$system_dir"/*) scanned=1 ;;
        esac
    fi

    copy_file "the VLC module" \
        "$REPO_ROOT/plugins/vlc/build/liblxlyrics_plugin.so" \
        "$dest_dir/liblxlyrics_plugin.so" 755 || {
        player_failed vlc "$copy_error"
        return 1
    }

    conf="$config_home/vlc/vlcrc"
    local state=""
    [ "$want_remember_state" -eq 0 ] && state=" lxlyrics-enabled=0"
    step "Configuring VLC: [lxlyrics] lxlyrics-app-path=$app_path$state"
    warn_if_running vlc "$conf"
    prepare_config_file "$conf"
    ini_set_key "$conf" "[lxlyrics]" lxlyrics-app-path "$app_path"
    if [ "$want_remember_state" -eq 0 ]; then
        ini_set_key "$conf" "[lxlyrics]" lxlyrics-enabled 0
    fi
    verify_ini_value "$conf" "[lxlyrics]" lxlyrics-app-path "$app_path"
    if [ "$want_remember_state" -eq 0 ]; then
        verify_ini_value "$conf" "[lxlyrics]" lxlyrics-enabled 0
    fi

    if [ "$scanned" -eq 1 ]; then
        # The module is where VLC looks, so the extra interface can be enabled
        # for every start (VLC reads extraintf once, at libvlc start).
        add_ini_list_token "$conf" "[core]" extraintf lxlyrics ":"
        case ":$(ini_get_key "$conf" "[core]" extraintf):" in
            *":lxlyrics:"*) ;;
            *) die "$conf: [core] extraintf must load lxlyrics" ;;
        esac
        summary_add_path vlc "$dest_dir/liblxlyrics_plugin.so"
        summary_add "" "vlcrc [core] extraintf / [lxlyrics] lxlyrics-app-path"
        next_steps_add vlc "restart VLC; the module is loaded through vlcrc extraintf=lxlyrics"
    else
        summary_add_path vlc "$dest_dir/liblxlyrics_plugin.so"
        summary_add "" "vlcrc [lxlyrics] lxlyrics-app-path"
        next_steps_add vlc "launch VLC with the module enabled:"
        next_steps_add "" "VLC_PLUGIN_PATH=$dest_dir vlc --extraintf=lxlyrics"
        if [ -n "$system_dir" ]; then
            next_steps_add "" "(VLC scans only $system_dir and \$VLC_PLUGIN_PATH;"
            next_steps_add "" "install into $system_dir/misc as root to drop the environment)"
        else
            next_steps_add "" "(VLC scans only its own plugin directory and \$VLC_PLUGIN_PATH)"
        fi
    fi
    if [ "$want_remember_state" -eq 0 ]; then
        next_steps_add "" "lxlyrics-enabled=0 is set: flip it in Preferences -> All -> Interface -> Main interfaces"
    fi
}

# --- Build + install the lyrics app ------------------------------------------------

if ! build_project "$APP_PROJECT" "$APP_DIR" "$APP_BINARY_NAME"; then
    die "$build_error"
fi

if ! copy_file "the lyrics app" "$APP_DIR/build/$APP_BINARY_NAME" "$app_path" 755; then
    die "$copy_error"
fi
app_pending=$copy_pending

# --- Install the requested adapters --------------------------------------------------

for player in "${ALL_PLAYERS[@]}"; do
    [ -n "${player_wanted[$player]:-}" ] || continue
    step "Installing the $player adapter"
    install_player_"$player" || true
done

# --- Summary --------------------------------------------------------------------------

step "Done"
printf '\n'
if [ "$app_pending" -eq 1 ]; then
    printf '  %-10s %s (needs root; see below)\n' "lyrics app" "$app_path"
else
    printf '  %-10s %s\n' "lyrics app" "$app_path"
fi
if [ "${#summary[@]}" -gt 0 ]; then
    printf '%s\n' "${summary[@]}"
fi

if [ "${#pending_root[@]}" -gt 0 ]; then
    printf '\nNeeds root (run these yourself, then restart the player):\n'
    for command in "${pending_root[@]}"; do
        printf '  sudo %s\n' "$command"
    done
fi

if [ "${#next_steps[@]}" -gt 0 ]; then
    printf '\nNext steps:\n'
    printf '%s\n' "${next_steps[@]}"
fi

if [ "${#problems[@]}" -gt 0 ]; then
    printf '\nNot installed:\n'
    for problem in "${problems[@]}"; do
        printf '  %s\n' "$problem"
    done
fi

if [ "${#problems[@]}" -gt 0 ] || [ "${#pending_root[@]}" -gt 0 ]; then
    printf '\n'
    exit 1
fi
