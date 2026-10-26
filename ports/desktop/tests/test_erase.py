import os
from harness import Run, check, drive
D = drive('import os, storage\nimport microcontroller\nif microcontroller.cpu.reset_reason != microcontroller.ResetReason.SOFTWARE:\n'
          '    storage.erase_filesystem()\nprint("after erase", sorted(os.listdir("/")))\n', {"keep.txt": "still here"}, "ERASE")
r = Run(["--drive", D])
ok = r.wait_for("after erase", 12)
r.stop()
check("erase_filesystem leaves the directory", ok and open(os.path.join(D, "keep.txt")).read() == "still here", r.text())
