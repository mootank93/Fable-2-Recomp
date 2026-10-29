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
REF = os.environ.get("FABLE2_REF", "v2.3.0")
EXPECTED_XEX_SHA = os.environ.get(
    "FABLE2_XEX_SHA256", "88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662")
HOME = Path.home()
PREFIX = Path(os.environ.get("FABLE2_PREFIX", HOME / ".local/share/fable2-port"))
CACHE = Path(os.environ.get("XDG_CACHE_HOME", HOME / ".cache")) / "fable2-setup"
LOG = CACHE / "setup.log"
NEED_GB = 25
MARKER = PREFIX / ".fable2-port-install"

# @@XISO@@
# @@ART@@

# ---------------------------------------------------------------- distro support
DEV_PACKAGES = {
    "fedora": ("dnf", "install -y", [
        "clang", "lld", "cmake", "ninja-build", "git", "gcc-c++", "make", "python3", "pkgconf-pkg-config",
        "vulkan-loader-devel", "mesa-vulkan-drivers", "wayland-devel", "wayland-protocols-devel",
        "libxkbcommon-devel", "libX11-devel", "libXext-devel", "libXrandr-devel", "libXcursor-devel",
        "libXfixes-devel", "libXi-devel", "libXtst-devel", "libxcb-devel", "libXScrnSaver-devel", "mesa-libEGL-devel", "alsa-lib-devel", "pulseaudio-libs-devel",
        "libdrm-devel", "mesa-libgbm-devel", "mesa-libGL-devel", "libdecor-devel", "dbus-devel",
        "systemd-devel", "libusb1-devel"]),
    "debian": ("apt-get", "install -y", [
        "build-essential", "clang", "lld", "cmake", "ninja-build", "git", "python3", "pkg-config",
        "libvulkan-dev", "mesa-vulkan-drivers", "libwayland-dev", "wayland-protocols", "libxkbcommon-dev",
        "libx11-dev", "libxext-dev", "libxrandr-dev", "libxcursor-dev", "libxfixes-dev", "libxi-dev",
        "libx11-xcb-dev", "libxcb1-dev", "libxtst-dev", "libegl1-mesa-dev", "libxss-dev", "libasound2-dev", "libpulse-dev", "libdrm-dev", "libgbm-dev", "libgl1-mesa-dev",
        "libdecor-0-dev", "libdbus-1-dev", "libudev-dev", "libusb-1.0-0-dev"]),
    "arch": ("pacman", "-S --needed --noconfirm", [
        "base-devel", "clang", "lld", "cmake", "ninja", "git", "python", "vulkan-headers",
        "vulkan-icd-loader", "mesa", "wayland", "wayland-protocols", "libxkbcommon", "libx11", "libxext",
        "libxrandr", "libxcursor", "libxfixes", "libxi", "libxcb", "libxtst", "libxss", "alsa-lib", "libpulse", "libdrm",
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


def packages_for(family):
    """DEV_PACKAGES[family], with Debian/Ubuntu switched to clang-20/lld-20 when the repository offers them.
    The game needs C++23 (std::expected), which Ubuntu 24.04's default clang 18 cannot do."""
    mgr, flags, pkgs = DEV_PACKAGES[family]
    if family == "debian":
        try:
            out = subprocess.run(["apt-cache", "policy", "clang-20"], capture_output=True, text=True).stdout
        except OSError:
            out = ""
        m = re.search(r"Candidate:\s*(\S+)", out)
        if m and m.group(1) != "(none)":
            pkgs = [("clang-20" if p == "clang" else "lld-20" if p == "lld" else p) for p in pkgs]
    return mgr, flags, pkgs


def clang_major(exe):
    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
    except OSError:
        return 0
    m = re.search(r"clang version (\d+)", out)
    return int(m.group(1)) if m else 0


def missing_packages(family):
    mgr, _, pkgs = packages_for(family)
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


def read_marker():
    """Info about the current install ({'version':..., 'game_mode':...}) or None when nothing is installed."""
    try:
        lines = MARKER.read_text().splitlines()
    except OSError:
        return None
    if not (PREFIX / "app/fable_2").is_file():
        return None
    return dict(l.split("=", 1) for l in lines if "=" in l)


def _vkey(tag):
    return tuple(int(x) for x in re.findall(r"\d+", tag))


def latest_ref(timeout=25):
    """Newest released version tag (vX.Y.Z) on GitHub, or None when offline."""
    if os.environ.get("FABLE2_REF"):
        return os.environ["FABLE2_REF"]
    try:
        out = subprocess.run(["git", "ls-remote", "--tags", "--refs", REPO_URL, "v*"], capture_output=True, text=True,
                             timeout=timeout, env=dict(os.environ, GIT_TERMINAL_PROMPT="0")).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    tags = [l.split("refs/tags/", 1)[1].strip() for l in out.splitlines() if "refs/tags/" in l]
    tags = [t for t in tags if re.fullmatch(r"v\d+(\.\d+)*", t)]
    return max(tags, key=_vkey) if tags else None


def uninstall(log, keep_saves=True, remove_cache=True):
    """Removes the install via its own uninstall.sh (never touches the original ISO/folder)."""
    script = PREFIX / "uninstall.sh"
    if not script.is_file():
        raise Fail("The uninstaller was not found in %s. Nothing was removed." % PREFIX)
    if subprocess.run(["pgrep", "-x", "fable_2"], capture_output=True).returncode == 0:
        raise Fail("Fable II is running. Close the game first, then try again.")
    cmd = ["bash", str(script), "--yes"] + (["--keep-saves"] if keep_saves else [])
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, FABLE2_PREFIX=str(PREFIX)))
    for line in (r.stdout + r.stderr).splitlines():
        log(line)
    if r.returncode != 0:
        raise Fail("Uninstall failed (exit code %d)." % r.returncode)
    if remove_cache:
        shutil.rmtree(CACHE / "src", ignore_errors=True)
        shutil.rmtree(CACHE / "payload", ignore_errors=True)


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


# ---------------------------------------------------------------- drive search
REAL_FS = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "jfs", "reiserfs", "ntfs", "ntfs3", "fuseblk", "vfat", "exfat",
           "udf", "iso9660", "nfs", "nfs4", "cifs", "zfs", "bcachefs"}
NOT_DRIVE_PREFIXES = ("/boot", "/var", "/snap", "/run/user", "/sys", "/proc", "/dev", "/run/host", "/etc", "/usr", "/tmp")
HARD_SKIP = {"/proc", "/sys", "/dev", "/run", "/usr", "/var", "/etc", "/boot", "/snap", "/opt", "/lib", "/lib32", "/lib64",
             "/libx32", "/bin", "/sbin", "/tmp", "/srv", "/root"}
SKIP_NAMES = {"node_modules", "__pycache__", "lost+found", "windows", "program files", "program files (x86)", "programdata",
              "$recycle.bin", "system volume information", "steamapps", "compatdata", "shadercache", "proton", "flatpak"}
ISO_MIN, ISO_MAX = int(os.environ.get("FABLE2_ISO_MIN", 500 * 1000 * 1000)), 16 * 1000 * 1000 * 1000


def _unescape_mount(s):
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), s)


def _disk_labels():
    out = {}
    d = Path("/dev/disk/by-label")
    try:
        for e in d.iterdir():
            try:
                out[str(e.resolve())] = _unescape_mount(e.name.replace("\\x20", " "))
            except OSError:
                pass
    except OSError:
        pass
    return out


def list_drives(mounts_text=None):
    """Drives the user can search: [{'label', 'roots', 'free_gb', 'total_gb'}]. The home folder always comes first."""
    if mounts_text is None:
        try:
            mounts_text = Path("/proc/mounts").read_text()
        except OSError:
            mounts_text = ""
    labels = _disk_labels()
    groups = {}
    for line in mounts_text.splitlines():
        f = line.split()
        if len(f) < 3 or f[2] not in REAL_FS:
            continue
        dev, mp = _unescape_mount(f[0]), _unescape_mount(f[1])
        if mp != "/" and mp.startswith(NOT_DRIVE_PREFIXES):
            continue
        groups.setdefault(dev, []).append(mp)
    out = [{"label": "Your home folder", "roots": [str(HOME)], "free_gb": None, "total_gb": None}]
    for dev, mps in groups.items():
        mps = sorted(set(mps), key=len)
        main = "/" in mps
        roots = ["/"] if main else mps
        try:
            u = shutil.disk_usage(roots[0])
            free, total = u.free / 1e9, u.total / 1e9
        except OSError:
            free = total = None
        name = labels.get(dev) or ("Main drive" if main else os.path.basename(roots[0].rstrip("/")) or dev)
        if main and name == "Main drive":
            name = "Main drive (everything except system folders)"
        out.append({"label": name, "roots": roots, "free_gb": free, "total_gb": total, "dev": dev})
    return out


def describe_drive(d):
    s = d["label"]
    if d.get("total_gb"):
        s += "   %.0f GB" % d["total_gb"] + (", %.0f GB free" % d["free_gb"] if d.get("free_gb") is not None else "")
    return s + "   (" + ", ".join(d["roots"]) + ")"


def scan_for_game(roots, on_progress=None, on_found=None, cancelled=lambda: False, max_depth=8, skip_mounts=(), expected=None):
    """Looks under `roots` for a Fable II disc image (.iso) or an extracted game folder (contains default.xex).
    Every find is checked against the supported version. Returns the list of finds:
    {'kind': 'iso'|'folder', 'path': str, 'ok': bool, 'note': str}"""
    expected = expected or EXPECTED_XEX_SHA
    results, seen = [], set()
    ndirs = 0
    last = [0.0]
    skip_here = {str(PREFIX), str(CACHE)} | set(skip_mounts)

    def report(path):
        if on_progress and time.time() - last[0] > 0.15:
            last[0] = time.time()
            on_progress(str(path), ndirs)

    def add(kind, path, ok, note):
        if path in seen:
            return
        seen.add(path)
        r = {"kind": kind, "path": path, "ok": ok, "note": note}
        results.append(r)
        if on_found:
            on_found(r)

    def check_iso(path):
        try:
            if not looks_like_xiso(path):
                return
            sha, _ = check_iso_xex(path)
        except (OSError, XisoError):
            return   # not a game disc (or unreadable): nothing worth showing
        if sha == expected:
            add("iso", path, True, "Fable II Game of the Year (supported version)")
        else:
            add("iso", path, False, "An Xbox 360 game, but not the supported Fable II edition")

    def check_folder(path, xex):
        try:
            sha = sha256_file(xex, cancelled)
        except (OSError, Cancelled):
            return
        if sha == expected:
            add("folder", path, True, "Extracted Fable II game folder (supported version)")
        else:
            add("folder", path, False, "Has a default.xex, but not the supported Fable II edition")

    stack = [(Path(r), 0) for r in reversed(list(roots))]
    while stack:
        if cancelled():
            break
        d, depth = stack.pop()
        sd = str(d)
        if sd in skip_here or (sd in HARD_SKIP):
            continue
        ndirs += 1
        report(d)
        try:
            entries = list(os.scandir(d))
        except OSError:
            continue
        subdirs, has_xex = [], None
        for e in entries:
            try:
                name = e.name
                low = name.lower()
                if e.is_dir(follow_symlinks=False):
                    if name.startswith(".") or low in SKIP_NAMES:
                        continue
                    subdirs.append(e.path)
                elif e.is_file(follow_symlinks=False):
                    if low == "default.xex":
                        has_xex = e.path
                    elif low.endswith(".iso") and ISO_MIN <= e.stat().st_size <= ISO_MAX:
                        if cancelled():
                            break
                        report(e.path)
                        check_iso(e.path)
            except OSError:
                continue
        if has_xex:
            check_folder(sd, has_xex)
            continue                      # the game folder itself: no need to look inside it
        if depth < max_depth:
            for sub in sorted(subdirs, reverse=True):
                stack.append((Path(sub), depth + 1))
    return results


def cli_find(path=None):
    """Terminal version of the drive search. Returns the chosen path or ''."""
    if path:
        roots, skip = [path], ()
        print("Searching %s ..." % path)
    else:
        drives = list_drives()
        print("Which drive should I search?")
        for i, d in enumerate(drives, 1):
            print("  %d) %s" % (i, describe_drive(d)))
        ans = input("Number > ").strip()
        if not (ans.isdigit() and 0 < int(ans) <= len(drives)):
            return ""
        d = drives[int(ans) - 1]
        roots = d["roots"]
        skip = tuple(r for x in drives if x is not d and x.get("dev") for r in x["roots"] if r != "/")
        print("Searching %s ..." % describe_drive(d))

    def prog(p, n):
        sys.stdout.write("\r  %d folders checked  %s" % (n, (p[-60:]).ljust(60)))
        sys.stdout.flush()
    res = scan_for_game(roots, on_progress=prog, skip_mounts=skip)
    sys.stdout.write("\r" + " " * 90 + "\r")
    if not res:
        print("Nothing found there. Try another drive, or type the path yourself.")
        return ""
    for i, r in enumerate(res, 1):
        print("  %d) %s  [%s]" % (i, r["path"], r["note"]))
    ans = input("Use which one? (number, or Enter to skip) > ").strip()
    if ans.isdigit() and 0 < int(ans) <= len(res):
        return res[int(ans) - 1]["path"]
    return ""


# ---------------------------------------------------------------- the installer
class Installer:
    """Runs the steps. UI-agnostic: it calls log(text), progress(fraction, label) and ask_password()."""

    def __init__(self, source, log, progress, use_gui_auth=False, keep_build=False, dry_run=False, ask_password=None, ref=None):
        self.source = Path(source).expanduser()
        self.log_cb, self.progress_cb = log, progress
        self.use_gui_auth, self.keep_build, self.dry = use_gui_auth, keep_build, dry_run
        self.ask_password = ask_password
        self.ref = ref
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
            self.check_compiler()
            return
        mgr, flags, _ = packages_for(fam)
        self.log("Missing packages: " + " ".join(miss))
        self.prog(0.3, "Installing build tools (you may be asked for your password)")
        cmd = self.sudo_prefix() + [mgr] + flags.split() + miss
        if mgr == "apt-get":
            cmd = self.sudo_prefix() + ["env", "DEBIAN_FRONTEND=noninteractive", mgr] + flags.split() + miss
        self.run(cmd)
        self.check_compiler()

    def check_compiler(self):
        """The game needs a recent clang (19 or newer) for C++23."""
        if self.dry:
            return
        exe = "clang++-20" if shutil.which("clang++-20") else "clang++"
        v = clang_major(exe)
        self.log("Compiler: %s (clang %s)" % (exe, v or "not found"))
        if v and v < 19:
            raise Fail("Your clang is version %d, but the game needs clang 19 or newer.\n"
                       "On Ubuntu/Debian install clang-20 and lld-20 (for older releases see https://apt.llvm.org), "
                       "then run the installer again." % v)

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
        ref = self.ref or latest_ref() or REF
        self.ref = ref
        self.log("Version: %s" % ref)
        if (src / ".git").is_dir():
            self.run(["git", "fetch", "--tags", "--force", "origin"], cwd=src)
            self.run(["git", "checkout", "-f", ref], cwd=src)
        else:
            shutil.rmtree(src, ignore_errors=True)
            self.run(["git", "clone", "--branch", ref, REPO_URL, str(src)])
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
        cache = B / "CMakeCache.txt"
        if cache.is_file() and not self.dry:
            want = "clang++-20" if shutil.which("clang++-20") else "clang++"
            try:
                m = re.search(r"^CMAKE_CXX_COMPILER:[A-Z]+=(.*)$", cache.read_text(errors="replace"), re.M)
            except OSError:
                m = None
            if m and os.path.basename(m.group(1)) != want:
                self.log("Compiler changed (%s -> %s); starting the build from scratch." % (os.path.basename(m.group(1)), want))
                shutil.rmtree(src / "out", ignore_errors=True)
                shutil.rmtree(src / "thirdparty/rexglue-sdk/out", ignore_errors=True)
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
        if not self.dry:
            try:
                commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=src, capture_output=True, text=True).stdout.strip()
                keep = [l for l in MARKER.read_text().splitlines() if l and not l.startswith(("version=", "commit="))]
                MARKER.write_text("\n".join(keep + ["version=%s" % self.ref, "commit=%s" % commit]) + "\n")
            except OSError as e:
                self.log("Could not record the installed version: %s" % e)
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
    info = read_marker()
    if args.uninstall or (info and not (args.iso or args.game or args.update)):
        if not info:
            print("Fable II is not installed here (%s)." % PREFIX); return 1
        action = "uninstall" if args.uninstall else None
        if not action:
            print("Fable II (Linux port) %s is installed in %s" % (info.get("version", "?"), PREFIX))
            ans = input("  1) Update   2) Uninstall   3) Quit   > ").strip()
            action = {"1": "update", "2": "uninstall"}.get(ans)
            if not action:
                return 0
        if action == "uninstall":
            keep = True
            if not args.yes:
                if input("Remove Fable II (Linux port)? Your original ISO is not touched. [y/N] ").strip().lower() != "y":
                    return 1
                keep = input("Keep a backup of your save files? [Y/n] ").strip().lower() != "n"
            else:
                keep = not args.delete_saves
            try:
                uninstall(print, keep_saves=keep)
            except Fail as e:
                print("FAILED:", e); return 1
            print("Uninstalled."); return 0
        args.update = True
    if args.update:
        if not info:
            print("Nothing to update: Fable II is not installed here."); return 1
        latest = latest_ref()
        if latest and info.get("version") == latest and not args.force:
            print("Already up to date (%s)." % latest); return 0
        print("Updating %s -> %s (your saves and settings are kept)" % (info.get("version", "?"), latest or "latest"))
        args.iso, args.game = None, str(PREFIX / "game")
    source = args.iso or args.game
    if not source:
        cands = find_candidates()
        print("Fable II installer. Where is your copy of the game?")
        for i, (k, v) in enumerate(cands, 1):
            print("  %d) %s: %s" % (i, k, v))
        ans = input("Type a number, the path to your .iso / game folder / disc, or F to search a drive: ").strip()
        if ans.lower() == "f":
            ans = cli_find()
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


# ---------------------------------------------------------------- native file pickers
def _run_picker(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else ""


def pick_path(kind, start=None, parent=None):
    """Opens the desktop's own file chooser. kind: 'iso' or 'folder'. Returns a path, "" if cancelled."""
    start = str(start or (HOME / "Downloads" if (HOME / "Downloads").is_dir() else HOME))
    title = "Select your Fable II disc image (.iso)" if kind == "iso" else "Select the folder that contains default.xex"
    on_kde = "KDE" in os.environ.get("XDG_CURRENT_DESKTOP", "").upper()
    if shutil.which("kdialog") and (on_kde or not shutil.which("zenity")):
        cmd = ["kdialog", "--title", title] + (["--getopenfilename", start + "/", "*.iso *.ISO|Disc images (*.iso)\n*|All files"]
                                              if kind == "iso" else ["--getexistingdirectory", start])
        r = _run_picker(cmd)
        if r is not None:
            return r
    if shutil.which("zenity"):
        cmd = ["zenity", "--file-selection", "--title=" + title, "--filename=" + start + "/"]
        if kind == "iso":
            cmd += ["--file-filter=Disc images | *.iso *.ISO", "--file-filter=All files | *"]
        else:
            cmd += ["--directory"]
        r = _run_picker(cmd)
        if r is not None:
            return r
    from tkinter import filedialog
    if kind == "iso":
        return filedialog.askopenfilename(parent=parent, title=title, initialdir=start,
                                          filetypes=[("Disc images", "*.iso *.ISO"), ("All files", "*")]) or ""
    return filedialog.askdirectory(parent=parent, title=title, initialdir=start) or ""


def ensure_tk():
    """True if tkinter is usable; offers to install it (one password prompt) when it's missing."""
    try:
        import tkinter  # noqa
        return True
    except ImportError:
        pass
    fam, _ = detect_family()
    pkg = {"fedora": "python3-tkinter", "debian": "python3-tk", "arch": "tk"}.get(fam)
    if not pkg or not sys.stdin.isatty():
        return False
    ans = input("The graphical installer needs %s. Install it now? [Y/n] " % pkg).strip().lower()
    if ans not in ("", "y", "yes"):
        return False
    mgr, flags, _ = DEV_PACKAGES[fam]
    cmd = (["sudo"] if os.geteuid() else []) + [mgr] + flags.split() + [pkg]
    if subprocess.run(cmd).returncode != 0:
        return False
    import importlib
    importlib.invalidate_caches()
    try:
        import tkinter  # noqa
        return True
    except ImportError:
        return False


# ---------------------------------------------------------------- graphical UI (Tk)
GOLD, GOLD_DIM, PANEL, TXT, TXT_DIM, GREEN = "#eec76b", "#9f8044", "#100d0a", "#d6cab0", "#9a9183", "#44881f"


def run_gui(args):
    import tkinter as tk
    from tkinter import messagebox, ttk
    root = tk.Tk()
    root.title("Fable II - Linux port installer")
    art = tk.PhotoImage(data=ART_B64)
    W, H = art.width(), art.height()
    root.geometry("%dx%d" % (W, H))
    root.resizable(False, False)
    cv = tk.Canvas(root, width=W, height=H, highlightthickness=0, bd=0)
    cv.pack()
    cv.create_image(0, 0, image=art, anchor="nw")
    bx0, by0, bx1, by1 = [int(v) for v in (ART_LAYOUT[0] * W, ART_LAYOUT[1] * H, ART_LAYOUT[2] * W, ART_LAYOUT[3] * H)]
    cv.create_rectangle(bx0, by0, bx1, by1, fill=PANEL, outline=GOLD, width=2)
    cv.create_rectangle(bx0 + 6, by0 + 6, bx1 - 6, by1 - 6, outline=GOLD_DIM, width=1)
    pad = 22
    x0, x1 = bx0 + pad, bx1 - pad
    inner = x1 - x0
    F = ("TkDefaultFont", 10)
    FB = ("TkDefaultFont", 11, "bold")

    title_id = cv.create_text((bx0 + bx1) // 2, by0 + 26, text="Install Fable II for Linux", fill=GOLD, font=("TkDefaultFont", 15, "bold"))
    sub_id = cv.create_text((bx0 + bx1) // 2, by0 + 48, anchor="n", width=inner, fill=TXT_DIM, font=("TkDefaultFont", 9), justify="center",
                   text="Choose your own copy of Fable II: Game of the Year Edition. About 25 GB of free space is needed.")

    def btn(text, cmd, primary=False, w=None):
        return tk.Button(root, text=text, command=cmd, relief="flat", bd=0, cursor="hand2", font=FB if primary else F,
                         bg=GREEN if primary else "#1d1812", fg="#fff6d8" if primary else TXT, activebackground="#58a82a" if primary else "#2b2318",
                         activeforeground="#ffffff", highlightthickness=2, highlightbackground=GOLD if primary else GOLD_DIM,
                         highlightcolor=GOLD, disabledforeground="#6d675c", padx=8, pady=4, width=w)

    src = tk.StringVar(value=args.iso or args.game or "")
    y = by0 + 92
    ent = tk.Entry(root, textvariable=src, bg="#1d1812", fg=TXT, insertbackground=TXT, relief="flat", font=F,
                   highlightthickness=1, highlightbackground=GOLD_DIM, highlightcolor=GOLD, readonlybackground="#1d1812")
    cv.create_window(x0, y, window=ent, anchor="nw", width=inner, height=30)
    b_iso = btn("Choose ISO...", lambda: choose("iso"), w=14)
    b_find = btn("Find my game...", lambda: open_finder(), w=14)
    b_dir = btn("Choose folder...", lambda: choose("folder"), w=14)
    cv.create_window(x0, y + 40, window=b_iso, anchor="nw")
    cv.create_window((x0 + x1) // 2, y + 40, window=b_find, anchor="n")
    cv.create_window(x1, y + 40, window=b_dir, anchor="ne")

    cands = find_candidates()
    box = None
    y2 = y + 86
    if cands:
        cv.create_text(x0, y2, text="Found on this computer:", anchor="nw", fill=TXT_DIM, font=("TkDefaultFont", 9))
        st = ttk.Style(root)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure("F.TCombobox", fieldbackground="#1d1812", background="#1d1812", foreground=TXT, arrowcolor=GOLD,
                     bordercolor=GOLD_DIM, lightcolor="#1d1812", darkcolor="#1d1812", selectbackground="#1d1812", selectforeground=TXT)
        st.map("F.TCombobox", fieldbackground=[("readonly", "#1d1812")], foreground=[("readonly", TXT)],
               selectbackground=[("readonly", "#1d1812")], selectforeground=[("readonly", TXT)])
        root.option_add("*TCombobox*Listbox.background", "#1d1812")
        root.option_add("*TCombobox*Listbox.foreground", TXT)
        root.option_add("*TCombobox*Listbox.selectBackground", GREEN)
        box = ttk.Combobox(root, values=["%s - %s" % c for c in cands], state="readonly", style="F.TCombobox")
        cv.create_window(x0, y2 + 18, window=box, anchor="nw", width=inner)
        box.bind("<<ComboboxSelected>>", lambda e: src.set(cands[box.current()][1]))
        y2 += 54
    go = btn("Install", lambda: start(), primary=True, w=16)
    cancel = btn("Cancel", lambda: state["inst"] and state["inst"].stop(), w=10)
    cancel.configure(state="disabled")
    cv.create_window(x0, y2 + 6, window=go, anchor="nw", height=44)
    cv.create_window(x1, y2 + 6, window=cancel, anchor="ne", height=44)

    py = y2 + 66
    cv.create_rectangle(x0, py, x1, py + 10, fill="#2a2218", outline="")
    fill = cv.create_rectangle(x0, py, x0, py + 10, fill=GOLD, outline="")
    status = cv.create_text(x0, py + 20, text="Ready.", anchor="nw", fill=TXT, font=F, width=inner)
    log_btn = btn("Show details", lambda: toggle_log(), w=12)
    cv.create_window(x0, by1 - 50, window=log_btn, anchor="sw")
    play = btn("Play now", lambda: subprocess.Popen([str(PREFIX / "fable2-launch")], start_new_session=True), primary=True, w=11)
    play.configure(state="disabled")
    cv.create_window(x1, by1 - 50, window=play, anchor="se")
    un = btn("Uninstall", lambda: do_uninstall(), w=10)
    un_win = cv.create_window((x0 + x1) // 2 - 22, by1 - 50, window=un, anchor="s")
    txt = tk.Text(root, state="disabled", wrap="none", font=("TkFixedFont", 8), bg="#0a0806", fg=TXT_DIM, relief="flat",
                  highlightthickness=1, highlightbackground=GOLD_DIM)
    log_win = cv.create_window(x0, py + 52, window=txt, anchor="nw", width=inner, height=by1 - 62 - (py + 52) - 36, state="hidden")
    q = queue.Queue()
    state = {"inst": None, "running": False, "log": False, "info": None, "latest": None}

    def refresh_ui():
        """Install / Update / Uninstall buttons follow what is actually installed."""
        info = read_marker()
        state["info"] = info
        busy = state["running"]
        if info:
            go.configure(text="Update", state="disabled" if busy else "normal")
            un.configure(state="disabled" if busy else "normal")
            play.configure(state="disabled" if busy else "normal")
            src.set(str(PREFIX / "game"))
            ent.configure(state="readonly")
            b_iso.configure(state="disabled"); b_dir.configure(state="disabled"); b_find.configure(state="disabled")
            if box is not None:
                box.configure(state="disabled")
            cv.itemconfigure(title_id, text="Fable II for Linux")
            cv.itemconfigure(sub_id, text="Fable II is installed on this computer. Update keeps your saves, settings and game files.")
            cv.itemconfigure(status, text="Installed: version %s. Checking for updates..." % info.get("version", "?"))
            threading.Thread(target=lambda: q.put(("latest", latest_ref())), daemon=True).start()
        else:
            go.configure(text="Install", state="disabled" if busy else "normal")
            un.configure(state="disabled")  # nothing to remove yet; stays visible so the option is discoverable
            play.configure(state="disabled")
            ent.configure(state="normal")
            if src.get() == str(PREFIX / "game"):
                src.set("")
            b_iso.configure(state="normal"); b_dir.configure(state="normal"); b_find.configure(state="normal")
            if box is not None:
                box.configure(state="readonly")
            cv.itemconfigure(title_id, text="Install Fable II for Linux")
            cv.itemconfigure(sub_id, text="Choose your own copy of Fable II: Game of the Year Edition. About 25 GB of free space is needed.")

    def do_uninstall():
        if not messagebox.askyesno("Uninstall", "Remove Fable II (Linux port) from this computer?\n\n"
                                   "Your original ISO or game folder is NOT touched."):
            return
        keep = messagebox.askyesno("Uninstall", "Keep a backup copy of your save files?\n\n"
                                   "Yes: copied to ~/Fable2-saves-backup-<date>\nNo: saves are deleted with the game.")
        state["running"] = True
        go.configure(state="disabled"); un.configure(state="disabled"); play.configure(state="disabled")
        cv.itemconfigure(status, text="Uninstalling...")

        def job():
            try:
                uninstall(lambda m: q.put(("log", m)), keep_saves=keep)
                q.put(("uninstalled", None))
            except Fail as e:
                q.put(("uninstalled", str(e)))
        threading.Thread(target=job, daemon=True).start()

    def toggle_log(force=None):
        state["log"] = (not state["log"]) if force is None else force
        cv.itemconfigure(log_win, state="normal" if state["log"] else "hidden")
        log_btn.configure(text="Hide details" if state["log"] else "Show details")


    def open_finder(auto_drive=None):
        """Window that searches a drive for the game: pick a drive, watch it search, use what it finds."""
        dlg = tk.Toplevel(root)
        dlg.title("Find my Fable II")
        dlg.configure(bg=PANEL)
        dlg.transient(root)
        dlg.resizable(False, False)
        DW, DH = 600, 350
        root.update_idletasks()
        dlg.geometry("%dx%d+%d+%d" % (DW, DH, root.winfo_rootx() + max(0, (W - DW) // 2), root.winfo_rooty() + max(0, (H - DH) // 2)))
        try:
            dlg.grab_set()
        except tk.TclError:
            pass

        def lbl(text, **kw):
            o = dict(bg=PANEL, fg=TXT, font=F, justify="left", anchor="w", wraplength=DW - 40)
            o.update(kw)
            return tk.Label(dlg, text=text, **o)

        def dbtn(text, cmd, primary=False, w=16):
            return tk.Button(dlg, text=text, command=cmd, relief="flat", bd=0, cursor="hand2", font=FB if primary else F,
                             bg=GREEN if primary else "#1d1812", fg="#fff6d8" if primary else TXT,
                             activebackground="#58a82a" if primary else "#2b2318", activeforeground="#ffffff",
                             highlightthickness=2, highlightbackground=GOLD if primary else GOLD_DIM, highlightcolor=GOLD,
                             disabledforeground="#6d675c", padx=8, pady=4, width=w)

        def listbox(h):
            return tk.Listbox(dlg, height=h, bg="#1d1812", fg=TXT, selectbackground=GREEN, selectforeground="#ffffff", relief="flat",
                              highlightthickness=1, highlightbackground=GOLD_DIM, highlightcolor=GOLD, activestyle="none",
                              font=F, exportselection=False)

        head = lbl("Find my copy of Fable II", fg=GOLD, font=("TkDefaultFont", 14, "bold"), anchor="center", justify="center")
        head.pack(pady=(14, 4), fill="x")
        msg = lbl("Pick the drive where your Fable II disc image (.iso) or game folder might be. I will look through it for you.")
        msg.pack(padx=20, fill="x")
        drives = list_drives()
        dl = listbox(7)
        for d in drives:
            dl.insert("end", describe_drive(d))
        dl.selection_set(0)
        dl.pack(padx=20, pady=10, fill="x")
        results = []
        rl = listbox(7)
        status_l = lbl("")
        row = tk.Frame(dlg, bg=PANEL)
        st = {"cancel": False, "thread": None, "closed": False}
        rq = queue.Queue()
        b_search = dbtn("Search this drive", lambda: begin(), primary=True, w=18)
        b_close = dbtn("Close", lambda: close(), w=10)
        b_stop = dbtn("Stop", lambda: stop(), w=10)
        b_use = dbtn("Use this one", lambda: use(), primary=True, w=13)
        b_other = dbtn("Search another drive", lambda: back(), w=18)
        b_hand = dbtn("Browse myself...", lambda: by_hand(), w=14)

        def show_pick():
            for w_ in (rl, status_l, row):
                w_.pack_forget()
            for w_ in (b_stop, b_use, b_other, b_hand):
                w_.pack_forget()
            msg.configure(text="Pick the drive where your Fable II disc image (.iso) or game folder might be. I will look through it for you.")
            dl.pack(padx=20, pady=10, fill="x")
            row.pack(pady=8)
            b_search.pack(in_=row, side="left", padx=6)
            b_close.pack(in_=row, side="left", padx=6)

        def begin():
            sel = dl.curselection()
            if not sel:
                return
            d = drives[sel[0]]
            skip = tuple(r for x in drives if x is not d and x.get("dev") for r in x["roots"] if r != "/")
            results.clear(); rl.delete(0, "end")
            st["cancel"] = False
            dl.pack_forget(); row.pack_forget(); b_search.pack_forget(); b_close.pack_forget()
            msg.configure(text="Searching %s ..." % d["label"])
            rl.pack(padx=20, pady=(10, 4), fill="x")
            status_l.configure(text="Starting...")
            status_l.pack(padx=20, fill="x")
            row.pack(pady=8)
            for w_ in (b_use, b_other, b_hand):
                w_.pack_forget()
            b_stop.pack(in_=row, side="left", padx=6)
            st["drive"] = d

            def work_():
                try:
                    scan_for_game(d["roots"], on_progress=lambda p, n: rq.put(("p", p, n)), on_found=lambda r: rq.put(("f", r)),
                                  cancelled=lambda: st["cancel"] or st["closed"], skip_mounts=skip)
                except Exception as e:       # never let the search crash the installer
                    rq.put(("e", str(e)))
                rq.put(("done",))
            st["thread"] = threading.Thread(target=work_, daemon=True)
            st["thread"].start()
            poll()

        def poll():
            if st["closed"]:
                return
            done = False
            try:
                while True:
                    m = rq.get_nowait()
                    if m[0] == "p":
                        status_l.configure(text="%d folders checked.  Now in: %s" % (m[2], ("..." + m[1][-48:]) if len(m[1]) > 51 else m[1]))
                    elif m[0] == "f":
                        r = m[1]
                        results.append(r)
                        rl.insert("end", ("Fable II (supported): " if r["ok"] else "Other Xbox game: ") + r["path"])
                        rl.itemconfigure("end", fg=GOLD if r["ok"] else TXT_DIM)
                        if r["ok"] and not rl.curselection():
                            rl.selection_set(rl.size() - 1)
                        b_use.configure(state="normal" if rl.curselection() and results[rl.curselection()[0]]["ok"] else "disabled")
                    elif m[0] == "e":
                        status_l.configure(text="The search stopped: " + m[1])
                    elif m[0] == "done":
                        done = True
            except queue.Empty:
                pass
            if done:
                finish()
            else:
                dlg.after(120, poll)

        def finish():
            b_stop.pack_forget()
            good = [r for r in results if r["ok"]]
            d = st["drive"]
            if good:
                msg.configure(text="Found your game." if len(good) == 1 else "Found %d copies. Pick the one you want." % len(good))
                status_l.configure(text="Select it and press Use this one.")
                if not rl.curselection():
                    rl.selection_set([i for i, r in enumerate(results) if r["ok"]][0])
                b_use.configure(state="normal")
                b_use.pack(in_=row, side="left", padx=6)
            else:
                others = len(results)
                msg.configure(text="I looked through %s but could not find Fable II." % d["label"])
                status_l.configure(text=("I did find %d other Xbox game disc(s), but not the supported Fable II edition. " % others if others else "")
                                   + "You can search another drive, or browse for the file yourself.")
            b_other.pack(in_=row, side="left", padx=6)
            b_hand.pack(in_=row, side="left", padx=6)

        def on_select(_e=None):
            sel = rl.curselection()
            if sel and sel[0] < len(results):
                b_use.configure(state="normal" if results[sel[0]]["ok"] else "disabled")

        rl.bind("<<ListboxSelect>>", on_select)

        def stop():
            st["cancel"] = True
            b_stop.configure(state="disabled")

        def use():
            sel = rl.curselection()
            if sel and results[sel[0]]["ok"]:
                src.set(results[sel[0]]["path"])
                close()

        def back():
            st["cancel"] = True
            show_pick()

        def by_hand():
            close()
            choose("iso")

        def close():
            st["closed"] = True
            try:
                dlg.grab_release()
            except tk.TclError:
                pass
            dlg.destroy()

        dlg.protocol("WM_DELETE_WINDOW", close)
        show_pick()
        if auto_drive is not None and auto_drive >= 0:       # used by the automated screenshot test
            dl.selection_clear(0, "end"); dl.selection_set(auto_drive); begin()
        return dlg

    def choose(kind):
        root.update_idletasks()
        p = pick_path(kind, parent=root)
        if p:
            src.set(p)

    def append(m):
        txt.configure(state="normal"); txt.insert("end", m + "\n"); txt.see("end"); txt.configure(state="disabled")

    def pump():
        try:
            while True:
                k, a = q.get_nowait()
                if k == "log":
                    append(a)
                elif k == "prog":
                    cv.coords(fill, x0, py, x0 + inner * a[0], py + 10)
                    if a[1]:
                        cv.itemconfigure(status, text=a[1])
                elif k == "latest":
                    state["latest"] = a
                    info = state["info"]
                    if info and not state["running"] and "Checking" in cv.itemcget(status, "text"):
                        cur = info.get("version", "?")
                        if a and a != cur:
                            cv.itemconfigure(status, text="Installed: %s. A new version is available: %s - press Update." % (cur, a))
                        elif a:
                            cv.itemconfigure(status, text="Installed: %s. You're up to date." % cur)
                        else:
                            cv.itemconfigure(status, text="Installed: %s. (Couldn't check for updates - offline?)" % cur)
                elif k == "uninstalled":
                    state["running"] = False
                    refresh_ui()
                    if a is None:
                        cv.itemconfigure(status, text="Uninstalled. Your original game copy was not touched."); cv.coords(fill, x0, py, x0, py + 10)
                    else:
                        cv.itemconfigure(status, text="Uninstall stopped: " + a.splitlines()[0][:90]); messagebox.showerror("Uninstall", a)
                elif k == "done":
                    state["running"] = False; cancel.configure(state="disabled")
                    if a is None:
                        refresh_ui()
                        cv.itemconfigure(status, text="Done! Start Fable II from your applications menu."); cv.coords(fill, x0, py, x1, py + 10)
                    else:
                        go.configure(state="normal"); refresh_ui()
                        cv.itemconfigure(status, text="Stopped: " + a.splitlines()[0][:90]); toggle_log(True)
                        messagebox.showerror("Installer", a)
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
        info = state["info"]
        if info:
            latest = state["latest"] or latest_ref()
            if latest and latest == info.get("version"):
                messagebox.showinfo("Update", "You already have the newest version (%s)." % latest)
                return
            if not messagebox.askyesno("Update", "Update to %s?\n\nYour saves, settings and game files are kept. "
                                       "The game is rebuilt on this computer, which takes a few minutes." % (latest or "the latest version")):
                return
            src.set(str(PREFIX / "game"))
        if not src.get().strip():
            choose("iso")
            if not src.get().strip():
                return
        inst = Installer(src.get().strip(), lambda m: q.put(("log", m)), lambda f, l: q.put(("prog", (f, l))),
                         use_gui_auth=True, keep_build=args.keep_build, dry_run=args.dry_run)
        state["inst"], state["running"] = inst, True
        go.configure(state="disabled"); un.configure(state="disabled"); cancel.configure(state="normal"); play.configure(state="disabled")
        threading.Thread(target=work, args=(inst,), daemon=True).start()

    def closing():
        if state["running"] and not messagebox.askyesno("Installer", "An install is running. Stop it and quit?"):
            return
        if state["inst"]:
            state["inst"].stop()
        root.destroy()
    root.protocol("WM_DELETE_WINDOW", closing)
    if getattr(args, "autostart", False):
        root.after(500, start)
    refresh_ui()
    if getattr(args, "autofind", None) is not None:
        root.after(600, lambda: open_finder(args.autofind))
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
    ap.add_argument("--update", action="store_true", help="update an existing install (keeps saves, settings and game files)")
    ap.add_argument("--uninstall", action="store_true", help="remove the installed port (keeps your original ISO)")
    ap.add_argument("--delete-saves", action="store_true", help="with --uninstall --yes: do not keep a backup of your saves")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    ap.add_argument("--force", action="store_true", help="with --update: rebuild even if already up to date")
    ap.add_argument("--find", nargs="?", const="", metavar="FOLDER", help="search a drive or folder for your game, then install it (without FOLDER: choose a drive)")
    ap.add_argument("--status", action="store_true", help="show what is installed and exit")
    ap.add_argument("--autostart", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--autofind", type=int, default=None, help=argparse.SUPPRESS)
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
    if a.find is not None and not (a.iso or a.game):
        found = cli_find(a.find or None)
        if not found:
            return 1
        if Path(found).is_dir():
            a.game = found
        else:
            a.iso = found
        a.cli = True
    if a.status:
        i = read_marker()
        print("Not installed." if not i else "Installed: version %s (%s) in %s" % (i.get("version", "?"), i.get("commit", "?"), PREFIX))
        return 0
    if sys.platform != "linux":
        print("This installer is for Linux."); return 1
    if os.uname().machine not in ("x86_64", "amd64"):
        print("This installer builds the x86-64 version. For ARM handhelds use the ArmadaOS package."); return 1
    if not a.cli and not (a.update or a.uninstall) and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        if ensure_tk():
            return run_gui(a)
        print("(The graphical installer needs python3-tk; continuing in the terminal.)")
    return run_cli(a)


if __name__ == "__main__":
    sys.exit(main())
