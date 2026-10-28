import pathlib
h = pathlib.Path(__file__).parent
x = (h / "xiso.py").read_text()
m = (h / "setup_main.py").read_text()
assert "# @@XISO@@" in m
(h.parent / "fable2-setup.py").write_text(m.replace("# @@XISO@@", x))
print("built fable2-setup.py")
