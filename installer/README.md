# One-file installer

`fable2-setup.py` builds, installs, updates and removes the Linux port, using **your own** copy of
Fable II (Game of the Year Edition). Nothing from the game is in this repository or downloaded by the installer.

    python3 fable2-setup.py              # window (needs python3-tk), otherwise the terminal
    python3 fable2-setup.py --cli --iso ~/Fable2.iso
    python3 fable2-setup.py --update     # update an existing install (keeps saves, settings, game files)
    python3 fable2-setup.py --uninstall  # remove it (asks whether to keep a backup of your saves)
    python3 fable2-setup.py --status
    python3 fable2-setup.py --test-iso ~/Fable2.iso    # just check an image and its version

It accepts an `.iso`, an already-extracted game folder, or a disc your drive can read (an Xbox 360 game
partition cannot be read by an ordinary PC DVD drive; rip it first with a suitable drive and use the ISO).

**Install:** check the game version -> install build packages (Fedora/Nobara, Debian/Ubuntu, Arch families;
asks for your password once) -> extract the ISO (pure Python, no extra tools) -> download the port's source
-> build -> install to `~/.local/share/fable2-port` with an applications-menu entry.
Needs about 25 GB free while it runs.

**Update:** when the installer is opened on an existing install it offers Update and Uninstall. Update
fetches the newest release, rebuilds, and replaces only the program files; your saves, launch-menu settings
and extracted game files are left alone.

**Uninstall:** removes the install and its menu entry. Your original ISO / game folder is never touched.
Saves can be kept as a backup in `~/Fable2-saves-backup-<date>`.

Developers: edit `src/`, then `python3 src/build_single.py` regenerates `fable2-setup.py`.
`python3 src/test_xiso.py` tests the ISO reader on synthetic images.
