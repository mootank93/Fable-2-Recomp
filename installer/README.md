# One-file installer

`fable2-setup.py` builds and installs the Linux port from **your own** copy of Fable II
(Game of the Year Edition). Nothing from the game is in this repository or downloaded by the installer.

    python3 fable2-setup.py            # window (needs python3-tk), otherwise the terminal
    python3 fable2-setup.py --cli --iso ~/Fable2.iso
    python3 fable2-setup.py --test-iso ~/Fable2.iso    # just check an image and its version

It accepts an `.iso`, an already-extracted game folder, or a disc your drive can read (an Xbox 360 game
partition cannot be read by an ordinary PC DVD drive; rip it first with a suitable drive and use the ISO).

Steps: check the game version -> install build packages (Fedora/Nobara, Debian/Ubuntu, Arch families;
asks for your password once) -> extract the ISO (pure Python, no extra tools) -> download the port's source
-> build (long) -> install to `~/.local/share/fable2-port` with an applications-menu entry.
Needs about 25 GB free while it runs. Remove with `~/.local/share/fable2-port/uninstall.sh`.

Developers: edit `src/`, then `python3 src/build_single.py` regenerates `fable2-setup.py`.
`python3 src/test_xiso.py` tests the ISO reader on synthetic images.
