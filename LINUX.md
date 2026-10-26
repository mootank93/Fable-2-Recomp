# Fable 2 recomp: Linux port

An experimental Linux (x86-64, Vulkan) build of the Fable 2 ReXGlue recompilation, with a pre-launch
settings menu, frame-rate options, a motion blur toggle, an FPS counter and save export/import.

> **You need your own copy of the game.** Nothing from Fable II is in this repository or in any
> release. The build translates *your own* `default.xex` into C++ on *your* machine. Do not share the
> resulting binaries. Use a legally obtained copy of the Fable 2 GOTY disc; see the main README for the
> supported version and its checksum.

**Status:** tested on Nobara/Fedora 44 (AMD Ryzen 9 7950X, Radeon RX 7900 XT, Wayland, Vulkan/RADV).
Other distributions and GPUs should work but are untested. An aarch64 build exists and boots on a
Snapdragon handheld, but has a texture colour glitch and is not supported yet.

## 1. Get the code

```
git clone --recursive -b linux-port https://github.com/mootank93/Fable-2-Recomp.git
cd Fable-2-Recomp
```

The `linux-port` branch pulls the SDK from `mootank93/rexglue-sdk` (branch `linux-fixes`), which carries
a few POSIX fixes that the Linux build needs.

## 2. Requirements

A C++20 toolchain and the usual Vulkan/desktop development packages. On Fedora:

```
sudo dnf install clang lld cmake ninja-build git vulkan-loader-devel mesa-vulkan-drivers
```

On Debian/Ubuntu the preset's `clang-20` is used if installed; otherwise any recent `clang` works (the build script picks
whichever it finds). Set `FABLE2_MARCH=-march=native` to tune for your own CPU (default: `-march=x86-64-v2`).

The SDK builds SDL3 and its other libraries itself; if the configure step names a missing system
package (X11/Wayland, ALSA/PulseAudio headers and similar), install it and re-run.

## 3. Put your game files in place

You need `default.xex` and the `data/` folder from your disc. Extract the disc image (`.iso`) with any
Xbox 360 extraction tool (for example [extract-xiso](https://github.com/XboxDev/extract-xiso)), then
either put the files in the repository folder or keep them anywhere and set `FABLE2_GAME_DIR`:

```
export FABLE2_GAME_DIR=$HOME/fable2/extracted     # folder that contains default.xex and data/
```

## 4. Build

```
linux/build.sh
```

The first build takes a while. It configures `linux-amd64-release`, builds the SDK, recompiles
`default.xex` and links `fable_2` into `out/build/linux-amd64-release/`.

## 5. Play

```
linux/play.sh
```

The **launch menu** appears first. Move with the arrow keys (or WASD, D-pad, left stick), change values
with Left/Right, confirm with Enter (or A). The mouse works too: click a row, click the right half to
change its value, right-click to go back.

| Setting | Notes |
| --- | --- |
| Preset | Custom, Original (Xbox 360), Enhanced |
| Render scale | 720p, 1440p, 4K, 5K (the console rendered at 720p) |
| Anisotropic filtering | Off to 16x |
| Frame rate | 30 (original), 60, 120, Display refresh. **60 and above are experimental.** |
| Motion blur | On (original) or Off |
| FPS counter | Off, On, Detailed |
| Fullscreen, VSync | |
| Save folder, Export saves, Import saves | Zips your saves for moving to another machine |

Choices are remembered in `fable2_launcher.conf` next to the executable (delete it to reset). Skip the
menu with `--no-launcher` or `FABLE2_NO_LAUNCHER=1`. Keyboard/mouse controls and the rest of the
configuration are described in the main README (`fable2_config.toml`).

### Moving saves between machines

Use **Export saves** on one install and **Import saves** on the other (copy the `.zip` across). The
profile id is fixed, so imported saves are picked up automatically. Importing backs up the saves it
replaces first. Saves live in `<build dir>/saves/`.

## Known issues

- **Startup race.** The game can crash on start-up on some CPUs unless it runs on two cores for its
  first seconds. `linux/play.sh` pins it to two cores for 25 seconds after you press Start, then
  releases it. Disable with `FABLE2_NO_PIN=1`.
- **First launch after a build shows a long white screen** while shaders are cached. Later launches are
  fast.
- **60/120 fps** looked correct in light testing (no speed-up or stutter noticed) but has not been
  tested thoroughly.
- **Not supported yet:** 4:3 output, HDR, aarch64/handheld builds.

## Credits

- [himdo/Fable-2-Recomp](https://github.com/himdo/Fable-2-Recomp), the Fable 2 recompilation project (MIT).
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, derived from Xenia (BSD).
- Linux port, launch menu and tooling: [mootank93](https://github.com/mootank93).
