#!/usr/bin/env bash
# WEB-16: run clang-tidy over every project translation unit in a configured build.
#
# Usage: scripts/run_clang_tidy.sh [build-dir] [file-regex]
#   build-dir   CMake build directory with compile_commands.json (default: build)
#   file-regex  only lint files whose path matches (default: everything under server/, common/
#               and tests/; GoogleTest sources under build/_deps are never linted)
#
# Checks live in .clang-tidy (repo root) and tests/.clang-tidy. Every enabled check is an error,
# so the exit status is non-zero on any finding.
#
# Environment:
#   RUN_CLANG_TIDY  run-clang-tidy driver (default: run-clang-tidy, e.g. run-clang-tidy-18 in CI)
#   CLANG_TIDY      clang-tidy binary     (default: clang-tidy,     e.g. clang-tidy-18 in CI)
#   JOBS            parallel jobs         (default: nproc)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(realpath -m "${1:-build}")"
file_regex="${2:-^${repo_root}/(server|common|tests)/}"

run_clang_tidy="${RUN_CLANG_TIDY:-run-clang-tidy}"
clang_tidy="${CLANG_TIDY:-clang-tidy}"
jobs="${JOBS:-$(nproc)}"

for tool in "$run_clang_tidy" "$clang_tidy"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' not found (set RUN_CLANG_TIDY / CLANG_TIDY)" >&2
        exit 2
    fi
done

if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "error: $build_dir/compile_commands.json missing; configure first:" >&2
    echo "  cmake -S . -B $build_dir -DCMAKE_BUILD_TYPE=Debug" >&2
    exit 2
fi

"$clang_tidy" --version | head -n 2
echo "build dir: $build_dir"
echo "files:     $file_regex"

# -quiet drops the per-file "N warnings generated" chatter; findings and their notes still print.
# run-clang-tidy exits non-zero when any clang-tidy invocation fails, which WarningsAsErrors causes.
cd "$repo_root"
"$run_clang_tidy" \
    -quiet \
    -p "$build_dir" \
    -clang-tidy-binary "$(command -v "$clang_tidy")" \
    -j "$jobs" \
    "$file_regex"
