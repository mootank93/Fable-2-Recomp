#!/usr/bin/env bash
# Build the Linux (x86-64) port.   usage: linux/build.sh      (JOBS=8 linux/build.sh to change parallelism)
# Needs your own default.xex in the game folder (see LINUX.md): the build translates it to C++.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
PRESET=linux-amd64-release
B=$ROOT/out/build/$PRESET
cd "$ROOT" || exit 1

[ -f thirdparty/rexglue-sdk/CMakeLists.txt ] || { echo "[build] SDK submodule missing. Run: git submodule update --init --recursive"; exit 1; }

if [ ! -f "$B/CMakeCache.txt" ]; then
  echo "[build] configuring ($PRESET)"
  cmake --preset "$PRESET" -DREXGLUE_ENABLE_FIDELITYFX=OFF || exit 1
fi

# Guard: a stale FidelityFX path in the cache compiles the SDK library with REX_HAS_FIDELITYFX_SDK
# while the game is built without it -> mismatched Presenter layout -> random crashes.
if grep -q "^REXGLUE_FIDELITYFX_SOURCE_DIR:INTERNAL=." "$B/CMakeCache.txt" 2>/dev/null &&
   grep -q "^REXGLUE_ENABLE_FIDELITYFX:BOOL=OFF" "$B/CMakeCache.txt"; then
  echo "[build] clearing stale REXGLUE_FIDELITYFX_SOURCE_DIR from the cache"
  cmake -S . -B "$B" -UREXGLUE_FIDELITYFX_SOURCE_DIR -DREXGLUE_ENABLE_FIDELITYFX=OFF > /dev/null || exit 1
fi

cmake --build "$B" --parallel "${JOBS:-$(nproc)}" 2>&1 | tail -5
rc=${PIPESTATUS[0]}
[ "$rc" = "0" ] || { echo "[build] FAILED"; exit "$rc"; }

n=$(ninja -C "$B" -t commands 2>/dev/null | grep "src/ui/presenter.cpp" | head -1 | grep -c REX_HAS_FIDELITYFX_SDK)
if [ "$n" != "0" ] && grep -q "^REXGLUE_ENABLE_FIDELITYFX:BOOL=OFF" "$B/CMakeCache.txt"; then
  echo "[build] WARNING: SDK presenter was compiled WITH the FidelityFX define but the option is OFF. Do not run this build."
  exit 2
fi
echo "[build] ok -> $B/fable_2"
