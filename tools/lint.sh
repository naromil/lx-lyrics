#!/usr/bin/env bash
#
# Style + static-analysis gate for both projects (this repo has no CI).
#
#   ./tools/lint.sh [--fix] [--format-only] [--tidy-only] [--help]
#
#   1. clang-format — checks every C++ source against the repo's .clang-format
#      (--fix rewrites the files instead of failing).
#   2. clang-tidy   — runs the curated .clang-tidy check set over both projects
#      with run-clang-tidy; findings fail the run.
#
# Prerequisites: clang-format, clang-tidy and run-clang-tidy on PATH; the tidy
# step needs a configured build dir per project (compile_commands.json).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

do_format=1
do_tidy=1
fix=0

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

step() {
    printf '\n==> %s\n' "$*"
}

usage() {
    cat <<'EOF'
usage: ./tools/lint.sh [--fix] [--format-only] [--tidy-only] [--help]

  --fix           apply clang-format in place instead of failing on differences
  --format-only   skip clang-tidy
  --tidy-only     skip clang-format
  --help          show this help and exit
EOF
}

require_command() {
    local command_name="$1"
    command -v "$command_name" >/dev/null 2>&1 \
        || die "required tool '$command_name' was not found on PATH (install it and re-run)"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --fix) fix=1 ;;
        --format-only) do_tidy=0 ;;
        --tidy-only) do_format=0 ;;
        --help|-h) usage; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

# --- clang-format -------------------------------------------------------------

run_format() {
    require_command clang-format

    local -a files
    mapfile -t files < <(
        find "$REPO_ROOT/lyrics-app/src" "$REPO_ROOT/lyrics-app/tests" "$REPO_ROOT/fooyin-plugin/src" \
            \( -name '*.cpp' -o -name '*.h' \) | sort
    )
    [ "${#files[@]}" -gt 0 ] || die "no C++ sources found"

    if [ "$fix" -eq 1 ]; then
        step "clang-format (rewriting ${#files[@]} files)"
        clang-format -i "${files[@]}"
    else
        step "clang-format (checking ${#files[@]} files)"
        clang-format --dry-run -Werror "${files[@]}"
    fi
}

# --- clang-tidy -----------------------------------------------------------------

run_tidy() {
    require_command clang-tidy
    require_command run-clang-tidy

    local project_dir db
    for project_dir in "$REPO_ROOT/lyrics-app" "$REPO_ROOT/fooyin-plugin"; do
        db="$project_dir/build/compile_commands.json"
        [ -f "$db" ] \
            || die "$db not found; configure the project first (cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug)"

        step "clang-tidy ($(basename "$project_dir"))"
        # Qt injects the GCC-only -mno-direct-extern-access flag through its
        # CMake targets; the clang driver behind clang-tidy rejects it, which
        # would fail every translation unit. The .clangd file strips the same
        # flag for the editor.
        run-clang-tidy \
            -p "$project_dir/build" \
            -j "$(nproc)" \
            -quiet \
            -removed-arg=-mno-direct-extern-access \
            -header-filter="^$project_dir/" \
            -source-filter="^$project_dir/(src|tests)/" \
            -warnings-as-errors='*' \
            "$project_dir/(src|tests)/.*\.cpp"
    done
}

[ "$do_format" -eq 1 ] && run_format
[ "$do_tidy" -eq 1 ] && run_tidy

step "Done"
