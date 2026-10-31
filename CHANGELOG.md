# Changelog

## 2.4.0 - Two-page menu and Debug mode
- Launch menu: now two pages, **Game** and **Extras and Debug**. Switch with LB / RB, Tab or PageUp / PageDown, or click a tab. Frame rate, VSync and the FPS counter are on page 1, where you change them most.
- Launch menu: new **Debug mode** row. It shows a live panel in the corner (game and display frame rate, frame-time graph, CPU, memory, threads) and turns on extra graphics logging (GPU debug markers, and the Vulkan validation layer when it is installed). Turning it on switches the FPS counter off, since the panel already shows the frame rate.
- Launch menu: new **Export debug report** row (needs Debug mode on). It writes one text file (system, graphics driver, your menu choices, a summary and the end of the last game log) into the save folder, so a bug report is a single file. Your home folder is hidden as `~`.
- Launch menu: now opens in a normal window instead of full screen. Your Full screen choice is applied when you press Start Game.
- Launch menu: rows that can't be used right now are shown greyed out, with a note on why.
- Launch menu: optional font. Put `launcher_font.ttf` next to the game program and the menu uses it.

## 2.3.2 - Controller fix
- Controllers: the game now picks up a controller that was already connected when it started. Before, the pad worked in the launch menu but not in play on some systems (seen on a handheld and inside a container).
- README: handheld (ARM64) status updated.

## 2.3.1 - Arch fix
- Installer: the build no longer inherits exported shell functions (such as the `module` helper). On Arch their error text made the build's configure step misread a working linker as broken.
- Installer: a retry after a failed configure now starts from a clean build folder instead of reusing the failed result.

## 2.3.0 - Find my game
- Installer: new **Find my game...** button. It lists your drives, searches the one you pick for a Fable II disc image (`.iso`) or extracted game folder, and checks each find against the supported version. If nothing turns up you can search another drive or browse by hand.
- Terminal: `--find [FOLDER]`, or answer `F` at the prompt.

## 2.2.1 - Ubuntu / Debian fixes
- Installer: the Debian/Ubuntu package list was missing `libxtst-dev`, `libx11-xcb-dev`, `libxcb1-dev` and `libegl1-mesa-dev`; the Fedora and Arch lists got the matching packages.
- Installer: the game needs clang 19 or newer (C++23). On Ubuntu/Debian it now installs `clang-20` and `lld-20`, checks the compiler version, and restarts the build from scratch if the compiler changed. Ubuntu 24.04 is now confirmed to build and run.

## 2.2 - Update and Uninstall
- The installer window now has Update and Uninstall buttons. Update fetches the newest release and rebuilds, keeping your saves, settings and game files. Uninstall removes the port (your original ISO is never touched) and can keep a backup of your saves.
- Command line: `--update`, `--uninstall`, `--status`.

## 2.1 - One-file installer
- `fable2-setup.py`: pick your own Fable II GOTY ISO (or extracted folder) in a window with a native file chooser; it checks the version, installs build tools, builds the port on your computer and installs it. Includes the launch-menu artwork.

## 2.0 - The Launcher Update
Game compatibility and performance are unchanged; this release is about the experience around the game.
- New launch menu: presets, render scale, anisotropic filtering, frame rate, motion blur, FPS counter, fullscreen, VSync
- Fully controller-navigable (d-pad / stick, A to select)
- Save export/import with a built-in folder browser (no typing); import lists every .zip
- Toggles for the website bonus items and Collectors Edition items
- Close the game without Alt+F4: hold Start + Select for 3 s (or Ctrl+Q)
- Experimental ARM64 handheld build (Retroid Pocket / ArmadaOS)
- Optional custom menu artwork (see assets/launcher)

## Screenshots

![Launch menu](docs/screenshots/menu.png)

![In game](docs/screenshots/ingame-1.png)

![Dialogue](docs/screenshots/ingame-2.png)

![Quit box](docs/screenshots/quit-box.png)
