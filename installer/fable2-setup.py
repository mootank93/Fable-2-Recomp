#!/usr/bin/env python3
"""Fable II (Linux recomp port) - one-file installer.

Give it YOUR OWN copy of the game (an .iso file, an extracted folder, or a disc your drive can read).
It checks the game version, installs the build tools your distro needs, builds the port on your machine,
installs it and adds a menu entry. Nothing from the game is downloaded or shipped by this installer.

  python3 fable2-setup.py                  graphical installer (falls back to the terminal)
  python3 fable2-setup.py --cli --iso ~/Fable2.iso
  python3 fable2-setup.py --test-iso ~/Fable2.iso     (just checks the image and the game version)
"""
import argparse, hashlib, os, queue, re, shutil, signal, subprocess, sys, threading, time
from pathlib import Path

REPO_URL = os.environ.get("FABLE2_REPO", "https://github.com/mootank93/Fable-2-Recomp.git")
REF = os.environ.get("FABLE2_REF", "v2.1.0")
EXPECTED_XEX_SHA = os.environ.get(
    "FABLE2_XEX_SHA256", "88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662")
HOME = Path.home()
PREFIX = Path(os.environ.get("FABLE2_PREFIX", HOME / ".local/share/fable2-port"))
CACHE = Path(os.environ.get("XDG_CACHE_HOME", HOME / ".cache")) / "fable2-setup"
LOG = CACHE / "setup.log"
NEED_GB = 25

# Pure-Python Xbox 360 / Xbox disc image (XDVDFS) reader. No dependencies.
import os, struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game partition starts inside a full disc dump (raw/redump style), and 0 for a trimmed image.
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)


class XisoError(Exception):
    pass


def find_partition(f):
    """Returns (base_offset, root_sector, root_size) or raises XisoError."""
    for base in PARTITION_OFFSETS:
        try:
            f.seek(base + 32 * SECTOR)
            vd = f.read(SECTOR)
        except OSError:
            continue
        if len(vd) >= 28 and vd[:20] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", vd, 20)
            if root_size and root_size < (1 << 28):
                return base, root_sector, root_size
    raise XisoError("This does not look like an Xbox / Xbox 360 disc image.")


def _walk(f, base, sector, size, prefix, out, seen):
    """Collects (path, is_dir, start_sector, size) for one directory table."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    if len(data) < size:
        raise XisoError("Disc image is truncated or damaged.")
    stack = [0]
    visited = set()
    while stack:
        off = stack.pop()
        if off in visited or off + 14 > len(data):
            continue
        visited.add(off)
        left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
            continue  # unused padding
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        if not name or name in (".", "..") or "/" in name or "\\" in name or "\0" in name:
            raise XisoError("Disc image contains an unsafe file name; refusing to extract.")
        path = prefix + name
        if attr & 0x10:
            key = (start, fsize)
            out.append((path + "/", True, start, fsize))
            if key not in seen and fsize:
                seen.add(key)
                _walk(f, base, start, fsize, path + "/", out, seen)
        else:
            out.append((path, False, start, fsize))
        if left not in (0, 0xFFFF):
            stack.append(left * 4)
        if right not in (0, 0xFFFF):
            stack.append(right * 4)


def list_entries(f):
    base, rs, rsz = find_partition(f)
    out = []
    _walk(f, base, rs, rsz, "", out, set())
    return base, out


def extract(src, dest, progress=None, cancelled=lambda: False):
    """src: path to an image or a readable device. Extracts into dest. Returns the number of files."""
    with open(src, "rb") as f:
        base, entries = list_entries(f)
        total = sum(e[3] for e in entries if not e[1]) or 1
        done = 0
        files = 0
        dest = os.path.abspath(dest)
        os.makedirs(dest, exist_ok=True)
        for path, is_dir, start, size in entries:
            target = os.path.abspath(os.path.join(dest, path))
            if not (target == dest or target.startswith(dest + os.sep)):
                raise XisoError("Unsafe path in disc image.")
            if is_dir:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            f.seek(base + start * SECTOR)
            left = size
            with open(target, "wb") as o:
                while left > 0:
                    if cancelled():
                        raise XisoError("Cancelled.")
                    chunk = f.read(min(left, 4 << 20))
                    if not chunk:
                        raise XisoError("Disc image ended early (damaged or incomplete).")
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done / total)
            files += 1
        return files


def looks_like_xiso(path):
    try:
        with open(path, "rb") as f:
            find_partition(f)
        return True
    except (OSError, XisoError):
        return False


# ---------------------------------------------------------------- distro support
DEV_PACKAGES = {
    "fedora": ("dnf", "install -y --setopt=strict=0", [
        "clang", "lld", "cmake", "ninja-build", "git", "gcc-c++", "make", "python3", "pkgconf-pkg-config",
        "vulkan-loader-devel", "mesa-vulkan-drivers", "wayland-devel", "wayland-protocols-devel",
        "libxkbcommon-devel", "libX11-devel", "libXext-devel", "libXrandr-devel", "libXcursor-devel",
        "libXfixes-devel", "libXi-devel", "libXScrnSaver-devel", "alsa-lib-devel", "pulseaudio-libs-devel",
        "libdrm-devel", "mesa-libgbm-devel", "mesa-libGL-devel", "libdecor-devel", "dbus-devel",
        "systemd-devel", "libusb1-devel"]),
    "debian": ("apt-get", "install -y", [
        "build-essential", "clang", "lld", "cmake", "ninja-build", "git", "python3", "pkg-config",
        "libvulkan-dev", "mesa-vulkan-drivers", "libwayland-dev", "wayland-protocols", "libxkbcommon-dev",
        "libx11-dev", "libxext-dev", "libxrandr-dev", "libxcursor-dev", "libxfixes-dev", "libxi-dev",
        "libxss-dev", "libasound2-dev", "libpulse-dev", "libdrm-dev", "libgbm-dev", "libgl1-mesa-dev",
        "libdecor-0-dev", "libdbus-1-dev", "libudev-dev", "libusb-1.0-0-dev"]),
    "arch": ("pacman", "-S --needed --noconfirm", [
        "base-devel", "clang", "lld", "cmake", "ninja", "git", "python", "vulkan-headers",
        "vulkan-icd-loader", "mesa", "wayland", "wayland-protocols", "libxkbcommon", "libx11", "libxext",
        "libxrandr", "libxcursor", "libxfixes", "libxi", "libxss", "alsa-lib", "libpulse", "libdrm",
        "libdecor", "dbus", "libusb"]),
}


def read_os_release():
    d = {}
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                d[k] = v.strip().strip('"')
    except OSError:
        pass
    return d


def detect_family(osr=None):
    """Returns (family, note). family is fedora/debian/arch, or None when unsupported."""
    osr = osr or read_os_release()
    ids = [osr.get("ID", "")] + osr.get("ID_LIKE", "").split()
    if osr.get("ID") in ("steamos", "bazzite", "silverblue", "kinoite", "nixos") or osr.get("VARIANT_ID", "").startswith(("silverblue", "kinoite", "sericea", "onyx")):
        return None, "This looks like an immutable distro (%s)." % osr.get("PRETTY_NAME", osr.get("ID", "?"))
    for i in ids:
        if i in ("fedora", "rhel", "nobara", "centos"):
            return "fedora", ""
        if i in ("debian", "ubuntu", "linuxmint", "pop"):
            return "debian", ""
        if i in ("arch", "manjaro", "endeavouros", "cachyos"):
            return "arch", ""
    return None, "Unrecognised distro (%s)." % osr.get("PRETTY_NAME", "?")


def missing_packages(family):
    mgr, _, pkgs = DEV_PACKAGES[family]
    miss = []
    for p in pkgs:
        if family == "fedora":
            ok = subprocess.run(["rpm", "-q", "--whatprovides", p], capture_output=True).returncode == 0
        elif family == "debian":
            r = subprocess.run(["dpkg-query", "-W", "-f=${Status}", p], capture_output=True, text=True)
            ok = "install ok installed" in r.stdout
        else:
            ok = subprocess.run(["pacman", "-Qq", p], capture_output=True).returncode == 0 or \
                 subprocess.run(["pacman", "-Qqg", p], capture_output=True).returncode == 0
        if not ok:
            miss.append(p)
    return miss


# ---------------------------------------------------------------- helpers
class Cancelled(Exception):
    pass


class Fail(Exception):
    pass


def sha256_file(path, cancelled=lambda: False):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            if cancelled():
                raise Cancelled()
            h.update(chunk)
    return h.hexdigest()


def find_xex_root(d):
    d = Path(d)
    if (d / "default.xex").is_file():
        return d
    try:
        for sub in sorted(d.iterdir()):
            if sub.is_dir() and (sub / "default.xex").is_file():
                return sub
    except OSError:
        pass
    return None


def check_iso_xex(iso):
    """Streams default.xex out of an image and returns its sha256 (nothing written to disk)."""
    with open(iso, "rb") as f:
        base, entries = list_entries(f)
        for path, is_dir, start, size in entries:
            if path.lower() == "default.xex" and not is_dir:
                f.seek(base + start * SECTOR)
                h = hashlib.sha256()
                left = size
                while left > 0:
                    c = f.read(min(left, 1 << 20))
                    if not c:
                        raise XisoError("default.xex is truncated")
                    h.update(c)
                    left -= len(c)
                return h.hexdigest(), len(entries)
    raise XisoError("No default.xex in this image - is it really Fable II?")


def free_gb(path):
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    return shutil.disk_usage(p).free / 1e9


def find_candidates():
    """Likely places for the game: ISOs in common folders, mounted discs / drives, optical devices."""
    out = []
    for d in (HOME / "Downloads", HOME, HOME / "Documents", HOME / "Games", Path("/run/media") / os.environ.get("USER", ""), Path("/media")):
        try:
            for p in list(d.glob("*.iso")) + list(d.glob("*/*.iso")) + list(d.glob("*.ISO")):
                out.append(("ISO file", str(p)))
        except OSError:
            pass
    for base in (Path("/run/media") / os.environ.get("USER", ""), Path("/media") / os.environ.get("USER", ""), Path("/mnt")):
        try:
            for m in base.iterdir():
                if find_xex_root(m):
                    out.append(("Mounted disc / folder", str(m)))
        except OSError:
            pass
    for dev in ("/dev/sr0", "/dev/sr1", "/dev/cdrom", "/dev/dvd"):
        if os.path.exists(dev) and os.access(dev, os.R_OK) and looks_like_xiso(dev):
            out.append(("Disc drive", dev))
    seen, res = set(), []
    for k, v in out:
        if v not in seen:
            seen.add(v); res.append((k, v))
    return res


# ---------------------------------------------------------------- the installer
class Installer:
    """Runs the steps. UI-agnostic: it calls log(text), progress(fraction, label) and ask_password()."""

    def __init__(self, source, log, progress, use_gui_auth=False, keep_build=False, dry_run=False, ask_password=None):
        self.source = Path(source).expanduser()
        self.log_cb, self.progress_cb = log, progress
        self.use_gui_auth, self.keep_build, self.dry = use_gui_auth, keep_build, dry_run
        self.ask_password = ask_password
        self.cancel = False
        self.proc = None
        CACHE.mkdir(parents=True, exist_ok=True)
        self.logf = open(LOG, "a")

    def log(self, msg):
        self.logf.write(msg + "\n"); self.logf.flush()
        self.log_cb(msg)

    def prog(self, f, label=None):
        self.progress_cb(max(0.0, min(1.0, f)), label)

    def cancelled(self):
        return self.cancel

    def stop(self):
        self.cancel = True
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass

    def run(self, cmd, cwd=None, env=None, watch=None, shell=False):
        """Runs a command, streaming its output into the log. Raises Fail on error."""
        self.log("$ " + (cmd if shell else " ".join(map(str, cmd))))
        if self.dry:
            return
        e = dict(os.environ, **(env or {}))
        self.proc = subprocess.Popen(cmd, cwd=cwd, env=e, shell=shell, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, errors="replace", start_new_session=True)
        stop = threading.Event()
        if watch:
            def poll():
                while not stop.wait(2.0):
                    watch()
            threading.Thread(target=poll, daemon=True).start()
        try:
            for line in self.proc.stdout:
                self.log(line.rstrip())
                if self.cancel:
                    break
        finally:
            stop.set()
        rc = self.proc.wait()
        if self.cancel:
            raise Cancelled()
        if rc != 0:
            raise Fail("A step failed (exit code %d). Details: %s" % (rc, LOG))

    # ---- steps
    def step_source(self):
        s = self.source
        self.prog(0.0, "Checking your game copy")
        if s.is_dir():
            root = find_xex_root(s)
            if not root:
                raise Fail("No default.xex found in %s. Pick the folder that contains your game files." % s)
            self.game_dir, self.mode = root, "folder"
            sha = sha256_file(root / "default.xex", self.cancelled)
        elif s.exists():
            try:
                sha, n = check_iso_xex(s)
            except XisoError as e:
                raise Fail(str(e))
            except PermissionError:
                raise Fail("Cannot read %s (permission denied)." % s)
            self.mode = "extract"
        else:
            raise Fail("Not found: %s" % s)
        if sha != EXPECTED_XEX_SHA:
            raise Fail("This is not the Fable II version the port was built for (the game's main file doesn't match).\n"
                       "You need the original Xbox 360 Fable II: Game of the Year Edition (retail disc / its ISO).\n"
                       "  expected %s\n  found    %s" % (EXPECTED_XEX_SHA[:16] + "...", sha[:16] + "..."))
        self.log("Game version OK (Fable II GOTY).")

    def step_space(self):
        need = NEED_GB if self.mode == "extract" else NEED_GB - 8
        for p in {PREFIX, CACHE}:
            if free_gb(p) < need:
                raise Fail("Not enough free disk space in %s: about %d GB needed, %.0f GB free." % (p, need, free_gb(p)))

    def sudo_prefix(self):
        if os.geteuid() == 0:
            return []
        if self.use_gui_auth and shutil.which("pkexec"):
            return ["pkexec"]
        return ["sudo"]

    def step_deps(self):
        self.prog(0.0, "Checking build tools")
        fam, note = detect_family()
        if not fam:
            self.log(note + " The installer can't install packages here automatically.")
            need = [t for t in ("clang", "cmake", "ninja", "git", "ld.lld") if not shutil.which(t)]
            if need:
                raise Fail(note + "\nInstall these yourself, then run the installer again:\n  clang, lld, cmake, ninja, git\n"
                           "plus the development packages for Vulkan, Wayland/X11, ALSA/PulseAudio. See LINUX.md in the repository.")
            self.log("Build tools found; continuing.")
            return
        miss = [] if self.dry else missing_packages(fam)
        if not miss:
            self.log("All build packages already installed.")
            return
        mgr, flags, _ = DEV_PACKAGES[fam]
        self.log("Missing packages: " + " ".join(miss))
        self.prog(0.3, "Installing build tools (you may be asked for your password)")
        cmd = self.sudo_prefix() + [mgr] + flags.split() + miss
        if mgr == "apt-get":
            cmd = self.sudo_prefix() + ["env", "DEBIAN_FRONTEND=noninteractive", mgr] + flags.split() + miss
        self.run(cmd)

    def step_extract(self):
        game = PREFIX / "game"
        marker = PREFIX / ".fable2-port-install"
        if (game / "default.xex").is_file() and self.mode == "extract" and not self.dry:
            if sha256_file(game / "default.xex", self.cancelled) == EXPECTED_XEX_SHA:
                self.log("Game files already extracted; reusing them.")
                return
        if self.mode == "folder":
            return
        self.prog(0.0, "Extracting the game (about 7 GB)")
        self.log("Extracting %s -> %s" % (self.source, game))
        if self.dry:
            return
        PREFIX.mkdir(parents=True, exist_ok=True)
        if not marker.exists():
            marker.write_text("game_mode=extracted\ngame_source=%s\n" % self.source)
        shutil.rmtree(game, ignore_errors=True)
        try:
            n = extract(str(self.source), str(game), lambda f: self.prog(f, "Extracting the game"), self.cancelled)
        except XisoError as e:
            if self.cancel:
                raise Cancelled()
            raise Fail(str(e))
        self.log("Extracted %d files." % n)
        if not (game / "default.xex").is_file() or sha256_file(game / "default.xex") != EXPECTED_XEX_SHA:
            raise Fail("Extraction finished but default.xex does not verify.")

    def step_source_tree(self):
        self.prog(0.0, "Downloading the port's source code")
        src = CACHE / "src"
        if (src / ".git").is_dir():
            self.run(["git", "fetch", "--tags", "--force", "origin"], cwd=src)
            self.run(["git", "checkout", "-f", REF], cwd=src)
        else:
            shutil.rmtree(src, ignore_errors=True)
            self.run(["git", "clone", "--branch", REF, REPO_URL, str(src)])
        self.prog(0.5, "Downloading the SDK")
        self.run(["git", "submodule", "update", "--init", "--recursive"], cwd=src)
        self.src = src

    def step_build(self):
        src = self.src
        gamedir = str(PREFIX / "game") if self.mode == "extract" else str(self.game_dir)
        B = src / "out/build/linux-amd64-release"
        t0 = time.time()

        def watch():
            n = 0
            try:
                n = sum(1 for _ in open(B / ".ninja_log")) - 1
            except OSError:
                pass
            self.prog(0.0 + min(0.95, max(0, n) / 1800.0), "Building the game: %d steps done, %d min" % (max(n, 0), (time.time() - t0) // 60))
        self.prog(0.0, "Building the game (this takes a while)")
        self.run([str(src / "linux/build.sh")], cwd=src, env={"FABLE2_GAME_DIR": gamedir}, watch=watch)
        if not self.dry and not (B / "fable_2").is_file():
            raise Fail("The build finished without producing fable_2. See " + str(LOG))
        self.build_dir = B

    def step_install(self):
        self.prog(0.0, "Installing")
        src, B = self.src, self.build_dir
        payload = CACHE / "payload"
        if not self.dry:
            shutil.rmtree(payload, ignore_errors=True)
            (payload / "bin").mkdir(parents=True); (payload / "lib").mkdir()
            shutil.copy2(B / "fable_2", payload / "bin/fable_2")
            if (B / "fable2_config.toml").is_file():
                shutil.copy2(B / "fable2_config.toml", payload / "fable2_config.toml")
            sdk = src / "thirdparty/rexglue-sdk/out"
            for lib in ("librexruntime.so", "librexgpu-xenos.so"):
                hits = sorted(sdk.rglob(lib))
                if not hits:
                    raise Fail("Missing %s after the build." % lib)
                shutil.copy2(hits[0].resolve(), payload / "lib" / lib)
            art = src / "assets/launcher"
            if art.is_dir():
                for f in art.iterdir():
                    if f.suffix in (".png", ".layout"):
                        shutil.copy2(f, payload / f.name)
        if self.mode == "extract" and not self.dry:
            PREFIX.mkdir(parents=True, exist_ok=True)
            mk = PREFIX / ".fable2-port-install"
            if not mk.exists():
                mk.write_text("game_mode=extracted\ngame_source=%s\n" % self.source)
        inst = src / "installer"
        cmd = ["bash", str(inst / "install.sh"), "--payload", str(payload)]
        if self.mode == "folder":
            cmd += ["--game", str(self.game_dir)]
        self.run(cmd, cwd=inst, env={"FABLE2_PREFIX": str(PREFIX), "FABLE2_IN_TERM": "1"})
        if not self.keep_build and not self.dry:
            self.log("Removing build files to save disk space...")
            shutil.rmtree(CACHE / "src/out", ignore_errors=True)
            shutil.rmtree(src / "thirdparty/rexglue-sdk/out", ignore_errors=True)
            shutil.rmtree(payload, ignore_errors=True)

    STEPS = [("Check game", "step_source", 0.03), ("Disk space", "step_space", 0.01),
             ("Build tools", "step_deps", 0.06), ("Extract game", "step_extract", 0.15),
             ("Source code", "step_source_tree", 0.05), ("Build", "step_build", 0.60),
             ("Install", "step_install", 0.10)]

    def run_all(self):
        done = 0.0
        total = sum(w for _, _, w in self.STEPS)
        for name, fn, w in self.STEPS:
            if self.cancel:
                raise Cancelled()
            self.log("== %s" % name)
            base = done
            outer = self.progress_cb

            def scaled(f, label=None, base=base, w=w):
                outer((base + f * w) / total, label or name)
            self.progress_cb = scaled
            try:
                getattr(self, fn)()
            finally:
                self.progress_cb = outer
            done += w
            outer(done / total, name + " done")
        self.log("All done.")


# ---------------------------------------------------------------- terminal UI
def run_cli(args):
    source = args.iso or args.game
    if not source:
        cands = find_candidates()
        print("Fable II installer. Where is your copy of the game?")
        for i, (k, v) in enumerate(cands, 1):
            print("  %d) %s: %s" % (i, k, v))
        ans = input("Type a number, or the path to your .iso / game folder / disc: ").strip()
        source = cands[int(ans) - 1][1] if ans.isdigit() and 0 < int(ans) <= len(cands) else ans
    last = [-1]

    def prog(f, label):
        pct = int(f * 100)
        if pct != last[0]:
            last[0] = pct
            print("\r" + ("[%3d%%] %s" % (pct, (label or "")[:70])).ljust(80), end="", flush=True)
    inst = Installer(source, lambda m: print("\n" + m if m.startswith("==") else "  " + m) if args.verbose or m.startswith(("==", "Game", "Extract", "Missing", "All")) else None,
                     prog, keep_build=args.keep_build, dry_run=args.dry_run)
    try:
        inst.run_all()
    except Cancelled:
        print("\nCancelled."); return 1
    except Fail as e:
        print("\n\nINSTALL FAILED:\n%s\nFull log: %s" % (e, LOG)); return 1
    print("\n\nInstalled. Start it from your applications menu (\"Fable 2 (Recomp port)\") or run %s/fable2-launch" % PREFIX)
    return 0


# ---------------------------------------------------------------- graphical UI (Tk; optional)
def run_gui(args):
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    root.title("Fable II - Linux port installer")
    root.geometry("760x640")
    q = queue.Queue()
    state = {"inst": None, "running": False}
    pad = dict(padx=14, pady=6)
    ttk.Label(root, text="Fable II - Linux port installer", font=("TkDefaultFont", 16, "bold")).pack(anchor="w", **pad)
    ttk.Label(root, wraplength=720, justify="left",
              text="Choose your own copy of Fable II: Game of the Year Edition - an .iso file, a folder with the extracted "
                   "game, or a disc your drive can read. The installer checks the version, installs build tools, builds the "
                   "port on this computer and adds it to your applications menu. Needs about 25 GB of free space "
                   "and takes a while.").pack(anchor="w", **pad)
    src = tk.StringVar(value=args.iso or args.game or "")
    row = ttk.Frame(root); row.pack(fill="x", **pad)
    ent = ttk.Entry(row, textvariable=src); ent.pack(side="left", fill="x", expand=True)
    ttk.Button(row, text="ISO file...", command=lambda: src.set(filedialog.askopenfilename(
        title="Select your Fable II .iso", filetypes=[("Disc images", "*.iso *.ISO"), ("All files", "*")]) or src.get())).pack(side="left", padx=4)
    ttk.Button(row, text="Folder...", command=lambda: src.set(filedialog.askdirectory(
        title="Select the folder containing default.xex") or src.get())).pack(side="left")
    cands = find_candidates()
    if cands:
        r2 = ttk.Frame(root); r2.pack(fill="x", **pad)
        ttk.Label(r2, text="Found:").pack(side="left")
        box = ttk.Combobox(r2, values=["%s - %s" % c for c in cands], state="readonly")
        box.pack(side="left", fill="x", expand=True, padx=6)
        box.bind("<<ComboboxSelected>>", lambda e: src.set(cands[box.current()][1]))
    bar = ttk.Progressbar(root, maximum=1000); bar.pack(fill="x", **pad)
    status = tk.StringVar(value="Ready.")
    ttk.Label(root, textvariable=status).pack(anchor="w", padx=14)
    txt = tk.Text(root, height=14, state="disabled", wrap="none", font=("TkFixedFont", 9)); txt.pack(fill="both", expand=True, **pad)
    btns = ttk.Frame(root); btns.pack(fill="x", **pad)
    go = ttk.Button(btns, text="Install"); go.pack(side="left")
    cancel = ttk.Button(btns, text="Cancel", state="disabled"); cancel.pack(side="left", padx=8)
    play = ttk.Button(btns, text="Play now", state="disabled"); play.pack(side="right")

    def append(m):
        txt.configure(state="normal"); txt.insert("end", m + "\n"); txt.see("end"); txt.configure(state="disabled")

    def pump():
        try:
            while True:
                k, a = q.get_nowait()
                if k == "log": append(a)
                elif k == "prog": bar["value"] = a[0] * 1000; status.set(a[1] or status.get())
                elif k == "done":
                    state["running"] = False; go.configure(state="normal"); cancel.configure(state="disabled")
                    if a is None:
                        status.set("Installed!"); bar["value"] = 1000; play.configure(state="normal")
                        messagebox.showinfo("Done", "Fable II is installed. Start it from your applications menu or press Play now.")
                    else:
                        status.set("Stopped."); messagebox.showerror("Installer", a)
        except queue.Empty:
            pass
        root.after(150, pump)

    def work(inst):
        try:
            inst.run_all(); q.put(("done", None))
        except Cancelled:
            q.put(("done", "Cancelled."))
        except Fail as e:
            q.put(("done", str(e)))
        except Exception as e:  # noqa
            q.put(("done", "Unexpected error: %r\nLog: %s" % (e, LOG)))

    def start():
        if not src.get().strip():
            messagebox.showwarning("Installer", "Choose your Fable II ISO or game folder first."); return
        inst = Installer(src.get().strip(), lambda m: q.put(("log", m)), lambda f, l: q.put(("prog", (f, l))),
                         use_gui_auth=True, keep_build=args.keep_build, dry_run=args.dry_run)
        state["inst"], state["running"] = inst, True
        go.configure(state="disabled"); cancel.configure(state="normal"); play.configure(state="disabled")
        threading.Thread(target=work, args=(inst,), daemon=True).start()

    go.configure(command=start)
    cancel.configure(command=lambda: state["inst"] and state["inst"].stop())
    play.configure(command=lambda: subprocess.Popen([str(PREFIX / "fable2-launch")], start_new_session=True))

    def closing():
        if state["running"] and not messagebox.askyesno("Installer", "An install is running. Stop it and quit?"):
            return
        if state["inst"]: state["inst"].stop()
        root.destroy()
    root.protocol("WM_DELETE_WINDOW", closing)
    pump(); root.mainloop()
    return 0


def main():
    ap = argparse.ArgumentParser(description="Fable II Linux port installer")
    ap.add_argument("--iso", help="path to your Fable II .iso, or a readable disc device")
    ap.add_argument("--game", help="folder with your extracted game files (contains default.xex)")
    ap.add_argument("--cli", action="store_true", help="terminal mode (no window)")
    ap.add_argument("--keep-build", action="store_true", help="keep the build folder (several GB) after installing")
    ap.add_argument("--dry-run", action="store_true", help="show the steps without changing anything")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--test-iso", metavar="PATH", help="only check an image: reads it and verifies the game version")
    a = ap.parse_args()
    if a.test_iso:
        try:
            sha, n = check_iso_xex(a.test_iso)
        except (XisoError, OSError) as e:
            print("FAILED:", e); return 1
        print("Image OK: %d entries. default.xex sha256 %s" % (n, sha))
        print("Matches the supported Fable II GOTY version." if sha == EXPECTED_XEX_SHA else "Does NOT match the supported version.")
        return 0 if sha == EXPECTED_XEX_SHA else 2
    if sys.platform != "linux":
        print("This installer is for Linux."); return 1
    if os.uname().machine not in ("x86_64", "amd64"):
        print("This installer builds the x86-64 version. For ARM handhelds use the ArmadaOS package."); return 1
    if not a.cli and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        try:
            import tkinter  # noqa
        except ImportError:
            print("(Graphical mode needs python3-tk; installing it is optional. Continuing in the terminal.)")
        else:
            return run_gui(a)
    return run_cli(a)


if __name__ == "__main__":
    sys.exit(main())
