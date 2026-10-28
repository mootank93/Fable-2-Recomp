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
