#!/usr/bin/env bash
#
# Build + install the LX Lyrics components, then auto-configure Fooyin so the
# plugin finds the lyrics app without any manual path entry.
#
#   ./tools/install.sh [--prefix DIR] [--no-autospawn] [--help]
#
# What it does:
#   1. Builds lyrics-app    -> lyrics-app/build/lx-lyrics-app
#   2. Builds plugins/fooyin -> plugins/fooyin/build/fyplugin_lxlyrics.so
#   3. Installs the app binary to <prefix>/bin
#   4. Installs the plugin to <data>/lib/fooyin/plugins
#   5. Patches the Fooyin settings file so [LxLyrics] AppPath points at the
#      installed binary and the plugin remembers the desktop-lyrics state
#      across sessions (RememberState=true; --no-autospawn writes false).
#
# Re-running the script is safe: cmake rebuilds incrementally and the config
# patch updates the existing [LxLyrics] keys in place (a stale AutoSpawn= line
# from the old plugin option is removed).
set -euo pipefail

# --- Configuration -----------------------------------------------------------

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LYRICS_APP_DIR="$REPO_ROOT/lyrics-app"
FOOYIN_PLUGIN_DIR="$REPO_ROOT/plugins/fooyin"

APP_BINARY_NAME="lx-lyrics-app"
PLUGIN_BINARY_NAME="fyplugin_lxlyrics.so"

CONF_SECTION="[LxLyrics]"
CONF_KEY_NAME="AppPath"
CONF_KEY_REMEMBER="RememberState"
# Setting key of the old AutoSpawn option: removed from [LxLyrics] so an
# existing fooyin.conf migrates cleanly.
CONF_KEY_OBSOLETE="AutoSpawn"

prefix="$HOME/.local"
prefix_given=0
want_remember_state=1

# --- Helpers -----------------------------------------------------------------

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

step() {
    printf '\n==> %s\n' "$*"
}

usage() {
    cat <<'EOF'
usage: ./tools/install.sh [--prefix DIR] [--no-autospawn] [--help]

  --prefix DIR    install the app under DIR/bin and the plugin under
                  DIR/lib/fooyin/plugins (default: $HOME/.local)
  --no-autospawn  write RememberState=false instead of =true (the flag name
                  is kept from the old AutoSpawn option): desktop lyrics
                  always start off and the last state is not restored
  --help          show this help and exit
EOF
}

require_command() {
    local command_name="$1"
    command -v "$command_name" >/dev/null 2>&1 \
        || die "required tool '$command_name' was not found on PATH (install it and re-run)"
}

# --- Parse command line -------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || die "--prefix requires a directory argument"
            prefix="$2"
            prefix_given=1
            shift 2
            ;;
        --prefix=*)
            prefix="${1#--prefix=}"
            [ -n "$prefix" ] || die "--prefix must not be empty"
            prefix_given=1
            shift
            ;;
        --no-autospawn)
            want_remember_state=0
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown argument '$1' (see --help)"
            ;;
    esac
done

# --- Resolve install locations -------------------------------------------------

if [ "$prefix_given" -eq 1 ]; then
    bin_dir="$prefix/bin"
    plugin_dir="$prefix/lib/fooyin/plugins"
else
    bin_dir="$HOME/.local/bin"
    plugin_dir="${XDG_DATA_HOME:-$HOME/.local}/lib/fooyin/plugins"
fi

conf_dir="${XDG_CONFIG_HOME:-$HOME/.config}/fooyin"
conf_file="$conf_dir/fooyin.conf"
app_path="$bin_dir/$APP_BINARY_NAME"

# --- Pre-flight checks ---------------------------------------------------------

require_command cmake
require_command ninja

[ -d "$LYRICS_APP_DIR" ] || die "lyrics-app source directory not found: $LYRICS_APP_DIR"
[ -d "$FOOYIN_PLUGIN_DIR" ] || die "plugins/fooyin source directory not found: $FOOYIN_PLUGIN_DIR"

# --- Build + install --------------------------------------------------------------

build_project() {
    local project_name="$1"
    local project_dir="$2"
    local artifact="$3"

    step "Building $project_name (Release)"
    (
        cd "$project_dir"
        cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build build
    ) || die "build of $project_name failed (see output above)"

    # A concurrent LSP/indexer may reconfigure the build dir and transiently
    # remove outputs; rebuild incrementally before giving up.
    if [ ! -e "$project_dir/build/$artifact" ]; then
        step "Artifact missing after build; rebuilding $project_name"
        (
            cd "$project_dir"
            cmake --build build
        ) || die "rebuild of $project_name failed (see output above)"
    fi
    [ -e "$project_dir/build/$artifact" ] || die "build of $project_name did not produce $artifact"
}

install_artifact() {
    local artifact_name="$1"
    local source_path="$2"
    local dest_path="$3"
    local dest_dir
    dest_dir="$(dirname "$dest_path")"

    step "Installing $artifact_name -> $dest_path"
    mkdir -p "$dest_dir" || die "cannot create install directory: $dest_dir"
    [ -w "$dest_dir" ] || die "install directory is not writable: $dest_dir"
    [ -e "$source_path" ] || die "build output is missing: $source_path"
    cp "$source_path" "$dest_path" || die "failed to copy $artifact_name to $dest_path"
    chmod 755 "$dest_path"
}

# Build, verify, and install each component in turn so the install copy happens
# immediately after its build completes.
build_project "lyrics-app" "$LYRICS_APP_DIR" "$APP_BINARY_NAME"
install_artifact "$APP_BINARY_NAME" "$LYRICS_APP_DIR/build/$APP_BINARY_NAME" "$app_path"

build_project "plugins/fooyin" "$FOOYIN_PLUGIN_DIR" "$PLUGIN_BINARY_NAME"
install_artifact "$PLUGIN_BINARY_NAME" "$FOOYIN_PLUGIN_DIR/build/$PLUGIN_BINARY_NAME" "$plugin_dir/$PLUGIN_BINARY_NAME"

# --- Patch the Fooyin settings file ----------------------------------------------

remember_value=false
if [ "$want_remember_state" -eq 1 ]; then
    remember_value=true
fi

step "Configuring Fooyin settings $CONF_SECTION: $CONF_KEY_NAME = $app_path, $CONF_KEY_REMEMBER = $remember_value"
mkdir -p "$conf_dir" || die "cannot create config directory: $conf_dir"
[ -w "$conf_dir" ] || die "config directory is not writable: $conf_dir"

patch_fooyin_config() {
    local value="$1"
    local remember="$2"
    local tmp_file="$conf_dir/.fooyin.conf.tmp.$$"

    if [ ! -f "$conf_file" ] || [ ! -s "$conf_file" ]; then
        # Fresh config: write just the [LxLyrics] section.
        {
            printf '%s\n' "$CONF_SECTION"
            printf '%s=%s\n' "$CONF_KEY_NAME" "$value"
            printf '%s=%s\n' "$CONF_KEY_REMEMBER" "$remember"
        } > "$conf_file" || die "failed to write $conf_file"
        return
    fi

    awk -v value="$value" \
        -v remember="$remember" \
        -v section="$CONF_SECTION" \
        -v key_name="$CONF_KEY_NAME" \
        -v remember_key="$CONF_KEY_REMEMBER" \
        -v obsolete_key="$CONF_KEY_OBSOLETE" '
        BEGIN { in_section = 0; section_seen = 0; key_seen = 0; remember_seen = 0 }
        /^\[/ {
            if (in_section) {
                in_section = 0
                if (!key_seen) print key_name "=" value
                if (!remember_seen) print remember_key "=" remember
            }
            if ($0 == section) {
                section_seen = 1
                in_section = 1
                key_seen = 0
                remember_seen = 0
            }
            print
            next
        }
        in_section {
            if ($0 ~ ("^" key_name "=")) { key_seen = 1; print key_name "=" value; next }
            if ($0 ~ ("^" remember_key "=")) { remember_seen = 1; print remember_key "=" remember; next }
            if ($0 ~ ("^" obsolete_key "=")) { next }  # stale AutoSpawn=...: drop it
            print
            next
        }
        { print }
        END {
            if (in_section) {
                if (!key_seen) print key_name "=" value
                if (!remember_seen) print remember_key "=" remember
            } else if (!section_seen) {
                print ""
                print section
                print key_name "=" value
                print remember_key "=" remember
            }
        }
    ' "$conf_file" > "$tmp_file" || die "failed to patch $conf_file"

    chmod --reference="$conf_file" "$tmp_file" 2>/dev/null || true
    mv -f "$tmp_file" "$conf_file" || die "failed to replace $conf_file"
}

verify_single_app_path() {
    local count
    count="$(awk -v section="$CONF_SECTION" -v key_name="$CONF_KEY_NAME" '
        /^\[/ { in_section = ($0 == section); next }
        in_section && $0 ~ ("^" key_name "=") { count++ }
        END { print count + 0 }
    ' "$conf_file")"
    [ "$count" -eq 1 ] || die "[$CONF_SECTION] in $conf_file must contain exactly one $CONF_KEY_NAME= line, found $count"
}

# The plugin keys the startup restore on the single [LxLyrics] RememberState
# line; a stale AutoSpawn= line must not survive the migration.
verify_remember_state() {
    local expected="$1"
    local summary count obsolete_count value
    summary="$(awk -v section="$CONF_SECTION" -v key="$CONF_KEY_REMEMBER" -v obsolete="$CONF_KEY_OBSOLETE" '
        /^\[/ { in_section = ($0 == section); next }
        in_section && $0 ~ ("^" key "=") { count++; value = substr($0, length(key) + 2) }
        in_section && $0 ~ ("^" obsolete "=") { obsolete_count++ }
        END { printf "%d %d %s\n", count + 0, obsolete_count + 0, value }
    ' "$conf_file")"
    read -r count obsolete_count value <<< "$summary"
    [ "$count" -eq 1 ] \
        || die "[$CONF_SECTION] in $conf_file must contain exactly one $CONF_KEY_REMEMBER= line, found $count"
    [ "$value" = "$expected" ] \
        || die "[$CONF_SECTION] $CONF_KEY_REMEMBER must be $expected, found '$value'"
    [ "$obsolete_count" -eq 0 ] \
        || die "[$CONF_SECTION] in $conf_file still contains the obsolete $CONF_KEY_OBSOLETE= key"
}

patch_fooyin_config "$app_path" "$remember_value"
verify_single_app_path
verify_remember_state "$remember_value"

# --- Summary --------------------------------------------------------------------

step "Done"
printf '\n'
printf '  Lyrics app binary : %s\n' "$app_path"
printf '  Fooyin plugin      : %s\n' "$plugin_dir/$PLUGIN_BINARY_NAME"
printf '  Plugin setting     : %s\n' "$CONF_SECTION $CONF_KEY_NAME=$app_path"
if [ "$want_remember_state" -eq 1 ]; then
    printf '  RememberState      : true (desktop lyrics state is restored; skip with --no-autospawn)\n'
else
    printf '  RememberState      : false (--no-autospawn; desktop lyrics always start off)\n'
fi
printf '\n'
printf 'Next steps:\n'
printf '  1. Restart Fooyin (or reload its plugins).\n'
printf '  2. Open View -> Desktop Lyrics to show the lyrics window.\n'
if [ "$want_remember_state" -eq 1 ]; then
    printf '     A fresh install stays OFF until that first toggle; later sessions restore it.\n'
fi
printf '  3. Tune options in Settings -> Lyrics -> LX Lyrics; the app path is already set.\n'
