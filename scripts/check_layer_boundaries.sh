#!/usr/bin/env bash
# Portable entry point for developers on Unix-like shells. CMake owns the
# rules so CI and local Windows builds execute exactly the same checks.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
args=()
if [ "${1:-}" = "--self-test" ]; then
    args+=(-DRUVIA_BOUNDARY_SELF_TEST=ON)
fi

exec cmake "${args[@]}" -P "$root/scripts/check_layer_boundaries.cmake"
