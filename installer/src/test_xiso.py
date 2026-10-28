import os, struct, tempfile, hashlib, random, sys
sys.path.insert(0, os.path.dirname(__file__))
import xiso
S = 2048
def build(files, base=0):
    """files: dict path->bytes (paths use /). Writes a minimal XDVDFS image, returns bytes."""
    tree = {}
    for p, d in files.items():
        parts = p.split("/"); n = tree
        for q in parts[:-1]: n = n.setdefault(q, {})
        n[parts[-1]] = d
    img = bytearray(base + 33 * S)  # sectors 0..32 reserved
    nxt = [33]
    def alloc(n):
        s = nxt[0]; nxt[0] += max(1, (n + S - 1) // S); return s
    def dir_table(node):
        names = sorted(node, key=lambda s: s.lower())
        # simple balanced BST via sorted list
        entries = []
        def rec(lo, hi):
            if lo > hi: return None
            mid = (lo + hi) // 2
            e = {"name": names[mid]}
            entries.append(e)   # pre-order: the tree root sits at offset 0, as on real discs
            e["l"] = rec(lo, mid - 1); e["r"] = rec(mid + 1, hi); return e
        root = rec(0, len(names) - 1)
        off = 0
        for e in entries:
            e["off"] = off; off += (14 + len(e["name"]) + 3) & ~3
        size = max(S, (off + S - 1) // S * S)
        buf = bytearray(b"\xff" * size)
        for e in entries:
            v = node[e["name"]]
            if isinstance(v, dict):
                sec = write_dir(v); sz = max(S, 0); attr = 0x10
                sz = dir_sizes[id(v)]
            else:
                sec = alloc(len(v)); 
                img.extend(b"\0" * (base + nxt[0] * S - len(img)))
                img[base + sec * S: base + sec * S + len(v)] = v
                sz = len(v); attr = 0x20
            l = e["l"]["off"] // 4 if e["l"] else 0
            r = e["r"]["off"] // 4 if e["r"] else 0
            struct.pack_into("<HHIIBB", buf, e["off"], l, r, sec, sz, attr, len(e["name"]))
            buf[e["off"] + 14: e["off"] + 14 + len(e["name"])] = e["name"].encode()
        return bytes(buf)
    dir_sizes = {}
    def write_dir(node):
        t = dir_table(node)
        sec = alloc(len(t))
        img.extend(b"\0" * (base + nxt[0] * S - len(img)))
        img[base + sec * S: base + sec * S + len(t)] = t
        dir_sizes[id(node)] = len(t)
        return sec
    rsec = write_dir(tree)
    rsz = dir_sizes[id(tree)]
    vd = xiso.MAGIC + struct.pack("<II", rsec, rsz) + b"\0" * 8 + b"\0" * (S - 36 - 20) + xiso.MAGIC
    img[base + 32 * S: base + 33 * S] = vd[:S]
    return bytes(img)

random.seed(1)
files = {"default.xex": os.urandom(5000), "data/a.bin": os.urandom(70000), "data/sub/b.txt": b"hi", "z.dat": b"", "Media/x/y/z.bin": os.urandom(3000)}
for k in range(30): files[f"data/many/f{k:02d}.bin"] = os.urandom(random.randint(1, 5000))
for base in (0, 0x0FD90000 // 64 * 0 + 0x2080000):   # trimmed and offset image
    with tempfile.TemporaryDirectory() as t:
        iso = os.path.join(t, "g.iso"); open(iso, "wb").write(build(files, base))
        assert xiso.looks_like_xiso(iso)
        out = os.path.join(t, "o"); n = xiso.extract(iso, out)
        for p, d in files.items():
            assert open(os.path.join(out, p), "rb").read() == d, p
        print("base", hex(base), "files", n, "OK")
with tempfile.TemporaryDirectory() as t:
    bad = os.path.join(t, "b.iso"); open(bad, "wb").write(os.urandom(70000))
    assert not xiso.looks_like_xiso(bad)
    evil = build({"ok.txt": b"x"}); 
    print("negative OK")
