import pathlib
h = pathlib.Path(__file__).parent
x = (h / "xiso.py").read_text()
a = (h / "art_data.py").read_text()
m = (h / "setup_main.py").read_text()
assert "# @@XISO@@" in m and "# @@ART@@" in m
(h.parent / "fable2-setup.py").write_text(m.replace("# @@XISO@@", x).replace("# @@ART@@", a))
print("built installer/fable2-setup.py")
