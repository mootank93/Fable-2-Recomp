#!/usr/bin/env bash
# Fable 2 (recompiled port) installer. User-level only: no root, nothing outside your home folder.
# Usage: ./install.sh --game /path/to/fable2.iso      (or a folder with the extracted game)
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${FABLE2_PREFIX:-$HOME/.local/share/fable2-port}
APPDIR=${XDG_DATA_HOME:-$HOME/.local/share}/applications
DESKTOP=$APPDIR/fable2-port.desktop
DESKTOP2=$APPDIR/fable2-port-settings.desktop
EXPECTED_SHA=${FABLE2_XEX_SHA256:-88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662}
PAYLOAD=$HERE/payload
GAME=""; COPY_GAME=0

# Double-click support: no terminal attached -> reopen in one, and keep it open at the end.
if [ ! -t 1 ] && [ -z "${FABLE2_IN_TERM:-}" ]; then
  export FABLE2_IN_TERM=1
  for t in konsole gnome-terminal xfce4-terminal xterm x-terminal-emulator; do
    if command -v "$t" >/dev/null 2>&1; then
      case $t in
        gnome-terminal) exec "$t" -- bash "$0" "$@";;
        *) exec "$t" -e bash "$0" "$@";;
      esac
    fi
  done
fi
pause() { if [ -n "${FABLE2_IN_TERM:-}" ]; then echo; read -r -p "Press Enter to close this window." _; fi; }
trap pause EXIT

die() { echo "ERROR: $*" >&2; exit 1; }
usage() { cat <<U
Usage: $0 --game PATH [--payload DIR] [--copy-game]
  --game PATH    your own copy of the game: an .iso file, or a folder containing default.xex
  --payload DIR  folder with the port files (default: ./payload next to this script)
  --copy-game    when PATH is a folder, copy it instead of linking to it (uses ~7 GB more)
Environment: FABLE2_PREFIX changes the install folder (default ~/.local/share/fable2-port)
Remove everything again with ./uninstall.sh
U
}
while [ $# -gt 0 ]; do
  case "$1" in
    --game) [ $# -ge 2 ] || die "--game needs a path"; GAME=$2; shift 2;;
    --payload) [ $# -ge 2 ] || die "--payload needs a folder"; PAYLOAD=$2; shift 2;;
    --copy-game) COPY_GAME=1; shift;;
    -h|--help) usage; exit 0;;
    *) usage; die "unknown option: $1";;
  esac
done

# Upgrade: with no --game (or --game pointing at the installed copy), keep the game files already installed.
REUSE=0
if [ -z "$GAME" ] && [ -f "$PREFIX/.fable2-port-install" ] && [ -f "$PREFIX/game/default.xex" ]; then
  REUSE=1; echo "Reusing the game files already installed in $PREFIX/game"
fi
if [ -n "$GAME" ] && [ -e "$PREFIX/game" ] && [ "$(readlink -f "$GAME" 2>/dev/null)" = "$(readlink -f "$PREFIX/game" 2>/dev/null)" ]; then
  REUSE=1
fi
if [ -z "$GAME" ] && [ "$REUSE" = 0 ]; then # look next to this script: an .iso, or a folder holding default.xex
  for c in "$HERE"/*.iso "$HERE"/*.ISO "$HERE"/game "$HERE"/extracted; do
    if [ -f "$c" ] || [ -f "$c/default.xex" ]; then GAME=$c; echo "Found game: $GAME"; break; fi
  done
fi
if [ -z "$GAME" ] && [ "$REUSE" = 0 ]; then
  if [ -t 0 ]; then read -r -p "Path to your Fable 2 .iso or extracted folder: " GAME; fi
  [ -n "$GAME" ] || { usage; die "no game given"; }
fi
GAME=${GAME/#\~/$HOME}

# --- checks before touching anything
case "$PREFIX" in ""|/|"$HOME"|"$HOME/") die "unsafe install folder: '$PREFIX'";; esac
# which CPU was the payload built for? (ELF e_machine: 0x3e = x86-64, 0xb7 = aarch64)
case "$(od -An -tx1 -j18 -N2 "$PAYLOAD/bin/fable_2" 2>/dev/null | tr -d ' \n')" in
  3e00) PAY=x86_64;; b700) PAY=aarch64;; *) PAY=unknown;; esac
HOST=$(uname -m)
if [ "$PAY" = unknown ]; then echo "note: could not tell which CPU the port was built for."
elif [ "$PAY" = "$HOST" ]; then echo "Port build: native $PAY."
else echo "note: this is a $HOST machine but the port is built for $PAY; it will run through an emulator, which is slower and may not work."; fi
[ -x "$PAYLOAD/bin/fable_2" ] || die "port files not found: $PAYLOAD/bin/fable_2 (see README: payload)"
ls "$PAYLOAD"/lib/librexruntime.so* >/dev/null 2>&1 || die "missing $PAYLOAD/lib/librexruntime.so"
command -v sha256sum >/dev/null || die "sha256sum not found"
[ "$REUSE" = 1 ] || [ -e "$GAME" ] || die "game not found: $GAME"
if [ -e "$PREFIX" ] && [ ! -f "$PREFIX/.fable2-port-install" ]; then
  die "$PREFIX already exists and was not made by this installer; refusing to touch it"
fi
UPGRADE=0; [ -f "$PREFIX/.fable2-port-install" ] && UPGRADE=1

# --- locate / extract the game
find_xex_root() { # dir -> prints dir that holds default.xex (top level or one level down)
  if [ -f "$1/default.xex" ]; then echo "$1"; return 0; fi
  local d; for d in "$1"/*/; do [ -f "${d}default.xex" ] && { echo "${d%/}"; return 0; }; done
  return 1
}
CREATED=0
cleanup_on_fail() {
  if [ "$CREATED" = 1 ] && [ "$UPGRADE" = 0 ]; then
    echo "Install failed; removing what was created."
    rm -rf "$PREFIX"; rm -f "$DESKTOP" "$DESKTOP2"
  fi
  pause
}
trap cleanup_on_fail EXIT
trap 'exit 130' INT TERM

mkdir -p "$PREFIX" || die "cannot create $PREFIX"
CREATED=1
touch "$PREFIX/.fable2-port-install"

GAME_MODE=""
if [ "$REUSE" = 1 ]; then
  GAME_MODE=$(sed -n 's/^game_mode=//p' "$PREFIX/.fable2-port-install"); [ -n "$GAME_MODE" ] || GAME_MODE=extracted
  GAME=$(sed -n 's/^game_source=//p' "$PREFIX/.fable2-port-install"); [ -n "$GAME" ] || GAME=$PREFIX/game
elif [ -d "$GAME" ]; then
  SRC=$(find_xex_root "$GAME") || die "no default.xex found in $GAME"
  if [ "$COPY_GAME" = 1 ]; then
    echo "Copying game files (about 7 GB)..."
    rm -rf "$PREFIX/game"; mkdir -p "$PREFIX/game"
    cp -a "$SRC"/. "$PREFIX/game/" || die "copy failed (disk full?)"
    GAME_MODE=copied
  else
    rm -rf "$PREFIX/game"; ln -s "$(readlink -f "$SRC")" "$PREFIX/game"; GAME_MODE=linked
  fi
else
  XISO=""
  for c in "$PAYLOAD/tools/extract-xiso" "$(command -v extract-xiso 2>/dev/null)"; do
    [ -n "$c" ] && [ -x "$c" ] && { XISO=$c; break; }
  done
  [ -n "$XISO" ] || die "$GAME is a file but extract-xiso is not available. Extract the ISO yourself and pass the folder."
  avail_kb=$(df -Pk "$PREFIX" | awk 'NR==2{print $4}')
  [ "${avail_kb:-0}" -gt 9000000 ] || die "need about 9 GB free in $PREFIX (have $((avail_kb/1024)) MB)"
  echo "Extracting the ISO (about 7 GB, a few minutes)..."
  rm -rf "$PREFIX/game"; mkdir -p "$PREFIX/game"
  "$XISO" -x "$GAME" -d "$PREFIX/game" >"$PREFIX/extract.log" 2>&1 || { tail -5 "$PREFIX/extract.log"; die "extraction failed"; }
  GAME_MODE=extracted
fi
XEX=$(find_xex_root "$PREFIX/game") || die "default.xex missing after setup"
if [ "$XEX" != "$PREFIX/game" ]; then # nested: point game at the inner folder
  inner=$XEX; [ "$GAME_MODE" = linked ] || { mv "$inner"/* "$inner"/.[!.]* "$PREFIX/game/" 2>/dev/null; }
fi
echo "Checking default.xex..."
have=$(sha256sum "$PREFIX/game/default.xex" | cut -d' ' -f1)
[ "$have" = "$EXPECTED_SHA" ] || die "default.xex does not match the version this port was built for.
  expected $EXPECTED_SHA
  found    $have
  (This port needs the original Xbox 360 retail/GOTY default.xex it was built from.)"

# --- install the port files (upgrades keep saves, cache and config edits of data folders)
echo "Installing port files..."
mkdir -p "$PREFIX/app/lib" "$PREFIX/app/data" "$PREFIX/app/logs" "$PREFIX/app/cache" "$PREFIX/app/saves" "$PREFIX/logs"
cp -a "$PAYLOAD/bin/fable_2" "$PREFIX/app/fable_2" || die "copy failed"
cp -a "$PAYLOAD"/lib/. "$PREFIX/app/lib/" || die "copy failed"
# the game looks for its GPU plugin next to the executable, so link every plugin there
for f in "$PREFIX"/app/lib/librexgpu-*.so; do [ -e "$f" ] && ln -sf "lib/$(basename "$f")" "$PREFIX/app/$(basename "$f")"; done
ls "$PREFIX"/app/librexgpu-xenos.so >/dev/null 2>&1 || die "librexgpu-xenos.so is missing from the payload (re-run make_bundle.sh)"
if [ -f "$PAYLOAD/fable2_config.toml" ] && { [ ! -f "$PREFIX/app/fable2_config.toml" ] || [ "$UPGRADE" = 0 ]; }; then
  cp -a "$PAYLOAD/fable2_config.toml" "$PREFIX/app/fable2_config.toml"
fi
for f in launcher_bg.png launcher_bg.layout launcher_logo.png; do
  [ -f "$PAYLOAD/$f" ] && cp -a "$PAYLOAD/$f" "$PREFIX/app/$f"
done
cp -a "$HERE/fable2-launch" "$PREFIX/fable2-launch"; chmod +x "$PREFIX/fable2-launch"
cp -a "$HERE/uninstall.sh" "$PREFIX/uninstall.sh"; chmod +x "$PREFIX/uninstall.sh"
# The in-game launch menu replaces the old preset script and its menu entry; clean them up on upgrade.
rm -f "$PREFIX/fable2-settings" "$DESKTOP2"
{ echo "game_mode=$GAME_MODE"; echo "game_source=$GAME"; echo "installed=$(date -Is)"; } > "$PREFIX/.fable2-port-install"

mkdir -p "$APPDIR"
cat > "$DESKTOP" <<D
[Desktop Entry]
Type=Application
Name=Fable 2 (Recomp port)
Comment=Fable 2 static recompilation port
Exec=$PREFIX/fable2-launch
Path=$PREFIX
Terminal=false
Categories=Game;
D
CREATED=0   # success: disarm rollback
cat <<DONE

Installed to $PREFIX
  Start:      $PREFIX/fable2-launch   (or the "Fable 2 (Recomp port)" menu entry)
  Settings:   in the launch menu that appears when the game starts (presets, render scale, frame rate, saves)
  Uninstall:  $PREFIX/uninstall.sh
  Log:        $PREFIX/logs/game.log
Your original game copy ($GAME) was not modified.
DONE
pause; trap - EXIT
