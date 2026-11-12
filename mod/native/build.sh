#!/usr/bin/env bash
# Needs zig 0.15.2: with that compiler the DLL is byte-reproducible (zig 0.17.0 builds a different one).
set -euo pipefail
SELF=$(readlink -f "$0")
cd "$(dirname "$SELF")"
if ! command -v zig >/dev/null; then
    exec nix shell nixpkgs#zig -c "$SELF" "$@"
fi
[ "$(zig version)" = "0.15.2" ] || echo "warning: zig $(zig version), the DLL is built with 0.15.2" >&2
export ZIG_GLOBAL_CACHE_DIR=$PWD/build/zig-cache ZIG_LOCAL_CACHE_DIR=$PWD/build/zig-cache
mkdir -p build
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Iinclude -shared -s \
    -o terminal_damage_probe.dll src/terminal_damage_probe.cpp
rm -f terminal_damage_probe.lib
echo "built $(pwd)/terminal_damage_probe.dll"
