"""How much Learning Guide code runs on this port: a survey, not part of the suite.

    python3 survey_learning_guides.py prepare GUIDES WORK [N] [SEED]
    node ../vscode/libinstall.js WORK/libdrive --from-code       # the libraries, from the bundle
    python3 survey_learning_guides.py run WORK BIN [SECONDS] [BOARD] [BOARD_FILE or a directory of them]
    python3 survey_learning_guides.py seed WORK         # results-virtual-auto.json from earlier runs
    python3 survey_learning_guides.py files GUIDES WORK # data files for programs that missed one
    python3 survey_learning_guides.py report WORK

GUIDES is a checkout of adafruit/Adafruit_Learning_System_Guides (the .py files are enough).
prepare picks N code.py files (default 200, seed 2026), copies each project's Python files into
WORK/drives/NNN, and writes WORK/libdrive/code.py importing every library they use, so the
library installer fetches them all once. run starts each program for SECONDS (default 10) with
lib/ pointing at WORK/libdrive/lib, JOBS (environment, default 1) at a time, and sorts what
happened; report prints the counts. N larger than the repository takes every code.py.
SETTINGS=1 gives a project that brings no settings.toml made-up Wi-Fi and Adafruit IO settings, and
any other name its code reads with getenv (a token, a city id) made up too.
"""

import json, os, random, re, shutil, subprocess, sys, time

IMPORT = re.compile(r"^\s*(?:import\s+([\w., ]+)|from\s+([\w.]+)\s+import\b)", re.M)


def imports(source):
    names = set()
    for plain, from_ in IMPORT.findall(source):
        if from_:
            if not from_.startswith("."):
                names.add(from_.split(".")[0])
        else:
            for part in plain.split(","):
                part = part.strip().split(" ")[0].split(".")[0]
                if part:
                    names.add(part)
    return names


def prepare(guides, work, n=200, seed=2026):
    found = []
    for root, dirs, files in os.walk(guides):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        if "code.py" in files:
            found.append(os.path.relpath(os.path.join(root, "code.py"), guides))
    found.sort()
    picked = random.Random(seed).sample(found, min(n, len(found)))
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "drives"))
    os.makedirs(os.path.join(work, "libdrive"))
    wanted = set()
    index = []
    for i, rel in enumerate(picked):
        src = os.path.dirname(os.path.join(guides, rel))
        drive = os.path.join(work, "drives", "%03d" % i)
        os.makedirs(drive)
        local = set()
        sources = []
        for name in os.listdir(src):
            path = os.path.join(src, name)
            if name.endswith(".py") and os.path.isfile(path):
                shutil.copy(path, drive)
                local.add(name[:-3])
                sources.append(open(path, encoding="utf-8", errors="replace").read())
            elif os.path.isdir(path) and os.path.exists(os.path.join(path, "__init__.py")):
                shutil.copytree(
                    path, os.path.join(drive, name), ignore=shutil.ignore_patterns("*.mpy")
                )
                local.add(name)
        for s in sources:
            wanted |= imports(s) - local
        index.append({"n": i, "guide": rel})
    with open(os.path.join(work, "libdrive", "code.py"), "w") as f:
        f.write("".join("import %s\n" % name for name in sorted(wanted)))
    json.dump(
        {"found": len(found), "seed": seed, "picked": index},
        open(os.path.join(work, "index.json"), "w"),
        indent=1,
    )
    print("code.py files:", len(found), "picked:", len(picked), "imports to look up:", len(wanted))


def classify(text, still_running):
    tb = text.rfind("Traceback (most recent call last)")
    if tb < 0:
        if "Code done running" in text:
            return "finished", ""
        return ("running" if still_running else "finished"), ""
    lines = [l.strip() for l in text[tb:].splitlines() if l.strip()]
    err = next(
        (l for l in lines[1:] if not l.startswith("File ") and not l.startswith("Traceback")),
        lines[-1],
    )
    m = re.match(r"ImportError: no module named '([\w.]+)'", err)
    if m:
        return "missing module", m.group(1)
    m = re.match(r"AttributeError: '?module'? object has no attribute '(\w+)'", err)
    if m:
        return "missing attribute", m.group(1)
    if "needs --board" in err or "needs --uart" in err:
        return "needs hardware", err.split(":", 1)[-1].strip()
    if re.match(r"OSError: \[Errno 2\]", err):
        return "missing file", err
    if "No I2C device" in err or "No pull up" in err:
        return "needs hardware", err
    return "other error", err


def _read(path):
    # WSL's view of a Windows drive fails a read now and then under load (ENODATA): try again.
    for attempt in range(5):
        try:
            return open(path, "rb").read().decode(errors="replace")
        except OSError:
            time.sleep(0.2)
    return ""


RAN = ("running", "finished")


# The board file a program is written for, from its project's path and the board names it uses.
def board_for(work, entry):
    drive = os.path.join(work, "drives", "%03d" % entry["n"])
    source = "".join(
        open(os.path.join(drive, f), encoding="utf-8", errors="replace").read()
        for f in os.listdir(drive)
        if f.endswith(".py")
    )
    path = entry["guide"].lower()
    if "magtag" in source.lower() or "magtag" in path:
        return "magtag"
    if "adafruit_clue" in source or re.search(r"(^|/|_)clue", path):
        return "clue"
    if (
        re.search(r"matrix.?portal", path, re.I)
        or "adafruit_matrixportal" in source
        or "MTX_" in source
    ):
        return "matrixportal"
    if "pygamer" in path or "pygamer" in source.lower():
        return "pygamer"
    if re.search(r"pybadge|edgebadge|pybadger", path) or "pybadger" in source:
        return "pybadge"
    if (
        "circuitplayground" in source
        or re.search(r"board\.(SLIDE_SWITCH|ACCELEROMETER_\w+)", source)
        or re.search(r"circuit_?playground|cpx|cpb|bluefruit", path)
    ):
        return "circuitplayground"
    if re.search(r"board\.GP\d+", source) or "pico" in path:
        return "pico"
    if re.search(r"trinket|gemma", path):
        return "trinket"
    return "feather"


# What a guide keeps in settings.toml, made up, for a project that brings none: its program gets
# past reading them and reaches the network (SETTINGS=1).
SETTINGS = """CIRCUITPY_WIFI_SSID = "survey"
CIRCUITPY_WIFI_PASSWORD = "survey-password"
ADAFRUIT_AIO_USERNAME = "survey"
ADAFRUIT_AIO_KEY = "aio_survey"
"""


def made_up_settings(drive):
    """SETTINGS, and every other name the project's code reads with getenv, made up: a number
    for an id, a count or a flag, else a word."""
    names = set()
    for root, _, files in os.walk(drive):
        if "lib" in os.path.relpath(root, drive).split(os.sep):
            continue
        for f in files:
            if f.endswith(".py"):
                names.update(
                    re.findall(
                        r"getenv\(\s*[\"'](\w+)[\"']",
                        open(os.path.join(root, f), errors="replace").read(),
                    )
                )
    text = SETTINGS
    for name in sorted(names):
        if not re.search(r"^%s\s*=" % name, text, re.M):
            number = re.search(r"id$|celsius|offset|count|interval|zip", name, re.I)
            text += "%s = %s\n" % (name, "1" if number else '"survey"')
    return text


def run(work, binary, seconds=10.0, board="virtual", board_file=None):
    """BOARD_FILE a directory: each program with the board file it is written for (board_for),
    the results kept as results-BOARD-auto.json; a program that ran there already is not run again
    (FRESH=1 runs them all)."""
    index = json.load(open(os.path.join(work, "index.json")))
    lib = os.path.join(work, "libdrive", "lib")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    auto = board_file is not None and os.path.isdir(board_file)
    tag = board + (
        "-auto"
        if auto
        else "-" + os.path.splitext(os.path.basename(board_file))[0]
        if board_file
        else ""
    )
    results_path = os.path.join(work, "results-%s.json" % tag)
    before = {}
    if auto and os.path.exists(results_path) and os.environ.get("FRESH") != "1":
        before = {r["n"]: r for r in json.load(open(results_path))}

    def one(entry):
        if before.get(entry["n"], {}).get("kind") in RAN:
            return before[entry["n"]]
        drive = os.path.join(work, "drives", "%03d" % entry["n"])
        if os.path.isdir(lib) and not os.path.exists(os.path.join(drive, "lib")):
            os.symlink(lib, os.path.join(drive, "lib"))
        settings = os.path.join(drive, "settings.toml")
        # Made up where the project brings none, or the made-up ones of an earlier run.
        if os.environ.get("SETTINGS") == "1" and (
            not os.path.exists(settings) or open(settings).read().startswith(SETTINGS)
        ):
            open(settings, "w").write(made_up_settings(drive))
        chosen = board_for(work, entry) if auto else None
        log_path = os.path.join(work, "log-%03d.txt" % entry["n"])
        with open(log_path, "wb") as log:
            extra = (
                [
                    "--board-file",
                    os.path.join(board_file, chosen + ".toml") if auto else board_file,
                ]
                if board_file
                else []
            )
            p = subprocess.Popen(
                [binary, "--drive", drive, "--board", board] + extra,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                env=env,
            )
            end = time.time() + seconds
            while time.time() < end and p.poll() is None:
                text = _read(log_path)
                if "Code done running" in text:
                    break
                time.sleep(0.2)
            still = p.poll() is None and "Code done running" not in _read(log_path)
            p.kill()
            p.wait()
        text = _read(log_path)
        kind, detail = classify(text, still)
        print("%03d %-17s %-40s %s" % (entry["n"], kind, detail[:40], entry["guide"]), flush=True)
        return dict(entry, kind=kind, detail=detail, board=chosen)

    # JOBS programs at a time: each sleeps most of its seconds, so a whole repository takes minutes.
    from concurrent.futures import ThreadPoolExecutor

    with ThreadPoolExecutor(int(os.environ.get("JOBS", "1"))) as pool:
        results = list(pool.map(one, index["picked"]))
    json.dump(results, open(results_path, "w"), indent=1)


def files(guides, work, board="virtual"):
    """The data files (images, fonts, sounds) of the projects whose program stopped on a missing
    file: their folders checked out in GUIDES (a sparse checkout of the .py files) and copied
    beside the program, but no lib/ and nothing over 8 MB. The next run runs them again."""
    results = json.load(open(os.path.join(work, "results-%s-auto.json" % board)))
    wanted = [r for r in results if r["kind"] == "missing file"]
    folders = sorted({os.path.dirname(r["guide"]) for r in wanted})
    subprocess.run(
        ["git", "-C", guides, "sparse-checkout", "add"] + ["/%s/**" % f for f in folders],
        check=True,
    )
    copied = 0
    for r in wanted:
        src = os.path.join(guides, os.path.dirname(r["guide"]))
        drive = os.path.join(work, "drives", "%03d" % r["n"])
        for root, dirs, names in os.walk(src):
            dirs[:] = [d for d in dirs if d != "lib" and not d.startswith(".")]
            for name in names:
                path = os.path.join(root, name)
                target = os.path.join(drive, os.path.relpath(path, src))
                if (
                    name.endswith(".py")
                    or os.path.exists(target)
                    or os.path.getsize(path) > 8 * 1024 * 1024
                ):
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copy(path, target)
                copied += 1
    print("projects", len(folders), "files copied", copied)


def seed(work, board="virtual"):
    """results-BOARD-auto.json from the runs with one board file each: a program that ran with any
    of them counts as run, the others as they fared with the board file they are written for."""
    runs = {}
    for name in os.listdir(work):
        m = re.match(r"results-%s-(\w+)\.json$" % board, name)
        if m and m.group(1) != "auto":
            runs[m.group(1)] = {r["n"]: r for r in json.load(open(os.path.join(work, name)))}
    index = json.load(open(os.path.join(work, "index.json")))
    results = []
    for entry in index["picked"]:
        chosen = board_for(work, entry)
        ran = next((r[entry["n"]] for r in runs.values() if r[entry["n"]]["kind"] in RAN), None)
        mine = runs.get(chosen, runs.get("feather"))[entry["n"]]
        results.append(dict(ran or mine, board=chosen))
    json.dump(results, open(os.path.join(work, "results-%s-auto.json" % board), "w"), indent=1)
    print(
        "seeded from",
        sorted(runs),
        "ran",
        sum(r["kind"] in RAN for r in results),
        "of",
        len(results),
    )


def report(work):
    from collections import Counter

    for name in sorted(os.listdir(work)):
        if not name.startswith("results-"):
            continue
        results = json.load(open(os.path.join(work, name)))
        print("==", name, len(results))
        for kind, count in Counter(r["kind"] for r in results).most_common():
            print("  %-17s %3d" % (kind, count))
        for kind in ("missing module", "missing attribute", "needs hardware", "other error"):
            c = Counter(r["detail"] for r in results if r["kind"] == kind)
            if c:
                print(" ", kind + ":", ", ".join("%s (%d)" % kv for kv in c.most_common(25)))


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "prepare":
        prepare(sys.argv[2], sys.argv[3], *(int(a) for a in sys.argv[4:6]))
    elif cmd == "run":
        run(
            sys.argv[2],
            sys.argv[3],
            float(sys.argv[4]) if len(sys.argv) > 4 else 10.0,
            sys.argv[5] if len(sys.argv) > 5 else "virtual",
            sys.argv[6] if len(sys.argv) > 6 else None,
        )
    elif cmd == "files":
        files(sys.argv[2], sys.argv[3])
    elif cmd == "seed":
        seed(sys.argv[2])
    elif cmd == "report":
        report(sys.argv[2])
