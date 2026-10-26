import os, shutil
from harness import Run, T, check


def run(image, expect):
    r = Run(["--drive", image])
    ok = r.wait_for(expect, 10)
    maps = "/proc/%d/maps" % r.p.pid
    mapped = os.path.exists(maps) and any(image in l for l in open(maps))
    r.stop()
    return ok, mapped, r.text()


if os.path.exists(os.path.join(T, "new.img")):
    os.remove(os.path.join(T, "new.img"))
ok, mapped, t = run("new.img", "Hello World!")
check("new .img created, formatted, code.py runs", ok and os.path.isfile(os.path.join(T, "new.img")), t)
if os.path.exists("/proc"):
    check("the image is memory-mapped", mapped)
ok, mapped, t = run("new.img", "Hello World!")
check("the .img is reused on a second start", ok, t)
DEMO = os.environ.get("DEMO_IMAGE", "")
if os.path.isfile(DEMO):
    shutil.copy(DEMO, os.path.join(T, "demo.img"))
    ok, mapped, t = run("demo.img", "code.py output:")
    check("existing demo image boots", ok, t)
