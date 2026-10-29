import hashlib, importlib.util, os, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import test_xiso
single = next(p for p in (os.path.join(here, "fable2-setup.py"), os.path.join(here, "..", "fable2-setup.py")) if os.path.exists(p))
spec = importlib.util.spec_from_file_location("setup_single", single)
m = importlib.util.module_from_spec(spec); sys.argv = ["x"]; spec.loader.exec_module(m)
m.ISO_MIN = 0                      # the test images are tiny
good_xex = os.urandom(5000); bad_xex = os.urandom(5000)
expected = hashlib.sha256(good_xex).hexdigest()
tmp = tempfile.mkdtemp()
def w(rel, data):
    p = os.path.join(tmp, rel); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "wb").write(data)
w("Downloads/Fable 2 GOTY.iso", test_xiso.build({"default.xex": good_xex, "data/a.bin": b"x" * 100}))
w("deep/a/b/c/d/Other game.iso", test_xiso.build({"default.xex": bad_xex}))
w("Games/extracted/default.xex", good_xex); w("Games/extracted/data/skip_me.iso", b"not looked at")
w("Games/wrong/default.xex", bad_xex)
w("junk/not-xbox.iso", b"hello" * 1000)          # a .iso that is not an Xbox image
w("junk/readme.txt", b"x")
w(".hidden/Fable hidden.iso", test_xiso.build({"default.xex": good_xex}))
w("node_modules/x/Fable nm.iso", test_xiso.build({"default.xex": good_xex}))
w("toodeep/1/2/3/4/5/6/7/8/9/Fable too deep.iso", test_xiso.build({"default.xex": good_xex}))
prog = []; found = []
res = m.scan_for_game([tmp], on_progress=lambda p, n: prog.append(n), on_found=found.append, expected=expected)
got = {os.path.relpath(r["path"], tmp): r["ok"] for r in res}
want = {"Downloads/Fable 2 GOTY.iso": True, "deep/a/b/c/d/Other game.iso": False, "Games/extracted": True, "Games/wrong": False}
assert got == want, (got, want)
assert len(found) == len(res)
# cancel stops it
n = [0]
def cancel():
    n[0] += 1; return n[0] > 3
r2 = m.scan_for_game([tmp], cancelled=cancel, expected=expected)
assert len(r2) < len(res)
# depth limit
r3 = m.scan_for_game([tmp], max_depth=2, expected=expected)
assert {os.path.relpath(r["path"], tmp) for r in r3} == {"Downloads/Fable 2 GOTY.iso", "Games/extracted", "Games/wrong"}, r3
# drives
txt = """/dev/nvme0n1p3 / btrfs rw 0 0
/dev/nvme0n1p3 /home btrfs rw 0 0
/dev/nvme0n1p2 /boot ext4 rw 0 0
/dev/sdb1 /run/media/mt93/My\\040Games exfat rw 0 0
tmpfs /run/user/1000 tmpfs rw 0 0
/dev/sda1 /mnt/data ntfs3 rw 0 0
"""
d = m.list_drives(txt)
assert d[0]["label"] == "Your home folder"
roots = sorted(sum((x["roots"] for x in d[1:]), []))
assert roots == ["/", "/mnt/data", "/run/media/mt93/My Games"], roots
print("drive list:", [m.describe_drive(x) for x in d])
print("scan tests OK (%d results, %d progress calls)" % (len(res), len(prog)))
