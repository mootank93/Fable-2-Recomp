#!/usr/bin/env bash
# Removes everything install.sh created. Never touches your original ISO / game folder.
# Usage: uninstall.sh [--keep-saves] [--yes] [--dry-run]
set -u
PREFIX=${FABLE2_PREFIX:-$HOME/.local/share/fable2-port}
DESKTOP=${XDG_DATA_HOME:-$HOME/.local/share}/applications/fable2-port.desktop
DESKTOP2=${XDG_DATA_HOME:-$HOME/.local/share}/applications/fable2-port-settings.desktop
KEEP=0; YES=0; DRY=0
for a in "$@"; do case $a in
  --keep-saves) KEEP=1;; --yes|-y) YES=1;; --dry-run) DRY=1;;
  -h|--help) sed -n '2,3p' "$0"; exit 0;;
  *) echo "unknown option $a"; exit 1;; esac; done
die() { echo "ERROR: $*" >&2; exit 1; }
run() { if [ "$DRY" = 1 ]; then echo "[dry-run] $*"; else "$@"; fi; }

case "$PREFIX" in ""|/|"$HOME"|"$HOME/") die "unsafe folder '$PREFIX'";; esac
if [ ! -f "$PREFIX/.fable2-port-install" ]; then
  echo "Nothing to remove: $PREFIX is not a Fable 2 port install."
  [ -f "$DESKTOP" ] && grep -q "fable2-port\|Fable 2 (Recomp" "$DESKTOP" && run rm -f "$DESKTOP" && echo "Removed stale menu entry."
  exit 0
fi
pgrep -x fable_2 >/dev/null 2>&1 && die "the game is running; close it first"
MODE=$(sed -n 's/^game_mode=//p' "$PREFIX/.fable2-port-install")

echo "This will remove:"
echo "  $PREFIX  (port files, logs, cache$( [ "$MODE" = extracted -o "$MODE" = copied ] && echo ", the game files it extracted/copied there"))"
echo "  $DESKTOP"
echo "It will NOT touch your original game copy$( [ "$MODE" = linked ] && echo " (the install only held a link to it)")."
SAVES=$PREFIX/app/saves
if [ -d "$SAVES" ] && [ -n "$(ls -A "$SAVES" 2>/dev/null)" ]; then
  if [ "$KEEP" = 0 ] && [ "$YES" = 0 ] && [ -t 0 ]; then
    read -r -p "Keep your save files (copied to ~/Fable2-saves-backup)? [Y/n] " r
    case $r in n|N) ;; *) KEEP=1;; esac
  fi
else KEEP=0; fi
if [ "$YES" = 0 ]; then
  [ -t 0 ] || die "not interactive; re-run with --yes"
  read -r -p "Continue? [y/N] " r; case $r in y|Y) ;; *) echo "Cancelled."; exit 1;; esac
fi

if [ "$KEEP" = 1 ]; then
  B=$HOME/Fable2-saves-backup-$(date +%Y%m%d-%H%M%S)
  run mkdir -p "$B" && run cp -a "$SAVES"/. "$B"/ || die "could not back up saves; nothing removed"
  echo "Saves kept in $B"
fi
run rm -f "$DESKTOP" "$DESKTOP2"
[ -L "$PREFIX/game" ] && run rm -f "$PREFIX/game"   # remove the link itself, never its target
run rm -rf "$PREFIX"
for f in /dev/shm/xenia_memory_*; do [ -O "$f" ] && run rm -f "$f"; done 2>/dev/null
echo "Uninstalled."
