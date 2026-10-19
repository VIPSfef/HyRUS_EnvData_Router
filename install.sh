#!/bin/sh
# Native Linux x64 build; default installs without admin privileges.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${BUILD_DIR:-"$project_dir/build/linux-x64"}
prefix=${PREFIX:-"$HOME/.local"}
cmake -S "$project_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF -DTB_TARGET_ARCH=x64
cmake --build "$build_dir" --parallel 2
cmake --install "$build_dir" --prefix "$prefix"
printf 'Installed: %s/bin/telemetry_send and telemetry_monitor (Linux x64)\n' "$prefix"
