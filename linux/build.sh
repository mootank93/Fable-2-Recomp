#!/usr/bin/env bash
# Build the Linux (x86-64) port.   usage: linux/build.sh      (JOBS=8 linux/build.sh to change parallelism)
# Needs your own default.xex in the game folder (see LINUX.md): the build translates it to C++.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
PRESET=linux-amd64-release
B=$ROOT/out/build/$PRESET
cd "$ROOT" || exit 1

[ -f thirdparty/rexglue-sdk/CMakeLists.txt ] || { echo "[build] SDK submodule missing. Run: git submodule update --init --recursive"; exit 1; }

if [ ! -f "$B/build.ninja" ]; then
  # The preset asks for clang-20 / clang++-20 (Debian/Ubuntu names). Fall back to plain clang elsewhere (Fedora, Arch...).
  if command -v clang++-20 >/dev/null 2>&1 && command -v clang-20 >/dev/null 2>&1; then CC=clang-20; CXX=clang++-20; else CC=clang; CXX=clang++; fi
  command -v "$CXX" >/dev/null 2>&1 || { echo "[build] clang not found. Install clang and lld (see LINUX.md)."; exit 1; }
  command -v ninja >/dev/null 2>&1 || { echo "[build] ninja not found. Install ninja (ninja-build) (see LINUX.md)."; exit 1; }
  MARCH=${FABLE2_MARCH:--march=x86-64-v2}      # the tested baseline; set FABLE2_MARCH=-march=native to tune for your CPU
  echo "[build] configuring ($PRESET) with $CXX $MARCH"
  cmake --preset "$PRESET" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$MARCH" -DCMAKE_CXX_FLAGS="$MARCH" \
    -DFABLE2_BUILD_PROFILE=goty-compatible -DREXSDK_DIR="$ROOT/thirdparty/rexglue-sdk" \
    -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=ON \
    || exit 1
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
