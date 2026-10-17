#!/bin/sh
# Build from a cloned source tree; default installs without administrator access.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${BUILD_DIR:-"$project_dir/build"}
prefix=${PREFIX:-"$HOME/.local"}
cmake -S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel 2
cmake --install "$build_dir" --prefix "$prefix"
printf 'Installed: %s/bin/telemetry_send\n' "$prefix"
