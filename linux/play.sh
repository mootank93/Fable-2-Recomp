#!/usr/bin/env bash
# Launch the Linux port.   usage: linux/play.sh [extra game options]
#   FABLE2_GAME_DIR   folder with your default.xex and data/   (default: the repo folder)
#   FABLE2_LOG        log file                                  (default: <build dir>/game.log)
#   FABLE2_NO_PIN=1   skip the startup core-pinning workaround
#   FABLE2_NO_LAUNCHER=1  skip the launch menu
# Startup workaround: for the first PIN_SECONDS after you press Start the game runs on 2 cores, which
# avoids a startup race; it is then released to all cores.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/out/build/linux-amd64-release
GAME=${FABLE2_GAME_DIR:-$ROOT}
PIN_SECONDS=${FABLE2_PIN_SECONDS:-25}
[ -x "$B/fable_2" ] || { echo "No build found. Run linux/build.sh first." >&2; exit 1; }
[ -f "$GAME/default.xex" ] || { echo "default.xex not found in $GAME. Set FABLE2_GAME_DIR to the folder holding your game files." >&2; exit 1; }

SDKLIB=$(dirname "$(readlink -f "$(find "$ROOT/thirdparty/rexglue-sdk/out" -name librexruntime.so 2>/dev/null | head -1)")")
RT="${XDG_RUNTIME_DIR:-/tmp}"

# Remove leftover shared-memory files from crashed runs (only when the game isn't running).
if ! pgrep -x fable_2 >/dev/null 2>&1; then
  for f in /dev/shm/xenia_memory_*; do [ -O "$f" ] && rm -f "$f"; done 2>/dev/null
fi
rm -f "$RT"/fable2-go.*

cd "$B" || exit 1
export LD_LIBRARY_PATH="$SDKLIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ARGS=(--game_data_root "$GAME" --log_file "${FABLE2_LOG:-$B/game.log}" --mnk_mode --mnk_mouse
      --no-vulkan_allow_present_mode_immediate --no-vulkan_allow_present_mode_mailbox)

if [ "${FABLE2_NO_PIN:-0}" = 1 ] || ! command -v taskset >/dev/null; then
  exec ./fable_2 "${ARGS[@]}" "$@"
fi

taskset -c 0,1 ./fable_2 "${ARGS[@]}" "$@" & pid=$!
trap 'kill $pid 2>/dev/null' INT TERM
(
  gpid=""
  for _ in $(seq 1 1200); do          # wait (up to 10 min) for the menu to be dismissed
    f=$(ls "$RT"/fable2-go.* 2>/dev/null | head -1)
    if [ -n "$f" ]; then gpid=${f##*.}; break; fi
    kill -0 "$pid" 2>/dev/null || exit 0
    sleep 0.5
  done
  sleep "$PIN_SECONDS"
  taskset -a -p -c "0-$(( $(nproc) - 1 ))" "${gpid:-$pid}" >/dev/null 2>&1
) & widener=$!
wait "$pid"; rc=$?
kill "$widener" 2>/dev/null; rm -f "$RT"/fable2-go.*
exit $rc
