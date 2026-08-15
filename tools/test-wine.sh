#!/bin/sh
# Run an already cross-built Windows test suite in an isolated prefix.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-"$root/.build/windows"}
export WINEPREFIX=${WINEPREFIX:-"$root/.build/wine-prefix"}
export WINEARCH=${WINEARCH:-win64}
export WINEDEBUG=${WINEDEBUG:--all}
ctest --test-dir "$build" --output-on-failure
