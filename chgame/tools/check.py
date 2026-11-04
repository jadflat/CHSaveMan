"""Everything that can be checked about a game without the board, in one go.

    chgame check [--quick] [--no-device]        (from the game's folder)
    chgame check --compare out/moments out/dev_moments
    python tools/check.py GAME [...]

The same for every game; tools/game.py says what the game adds (the schema
is in tools/gamecfg.py):

  0. The game's own preparations (PRE_STEPS, before_check): CHWordWheel's
     bank, CHCrossword's puzzle packs.
  1. The host tests (chgame test), and anything a game checks against them
     (after_host_tests: CHBackgammon's network in the simulator).
  2. Every script in tools/scripts runs in the simulator twice: the two runs
     must draw identical frames (the game is deterministic), with no drawing
     into a frame still being sent (the simulator's BUG lines). Scripts named
     device_* (the board only) and scripts that run on wall-clock time
     (free, freegif) are left out or run once; CARD scripts get the game's
     SD card.
  3. The redraw check (chgame redraw) for every script in tools/scripts/diff:
     the incremental redraw against a build that redraws everything.
  4. The game's simulator tests (SIM_TESTS: saving across a power cycle).
  5. The release build compiles for the device and fits (tools/check_size.py),
     and prints what the game requires of it (BUILD_REQUIRE).

Screenshots, contact sheets and GIFs are left in out/<script>/ to look at.
A section with nothing to do says `none`. Exit code 0 when everything
passed, 1 when something failed, 2 for a bad tools/game.py.

--compare A B: the images two runs of a script left (say the simulator's and
the board's, from `chgame run --device`): the same frames, pixel for pixel?
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "chsim"))
import gamecfg  # noqa: E402
import hosttests  # noqa: E402
import paths  # noqa: E402

FREE = re.compile(r"^\s*(free|freegif)\b", re.M)
ECHO_ALWAYS = ("perf", "calibration")


def frames_hash(path):
    """A hash of every frame's pixels (not of the file's bytes)."""
    from PIL import Image
    h = hashlib.sha256()
    im = Image.open(path)
    try:
        while True:
            h.update(im.convert("RGB").tobytes())
            im.seek(im.tell() + 1)
    except EOFError:
        pass
    return h.hexdigest()


def compare(one, two):
    one, two = Path(one), Path(two)
    files = sorted(p.name for p in one.iterdir() if p.suffix in (".png", ".gif") and p.name != "sheet.png")
    diff = [f for f in files if not (two / f).exists() or frames_hash(one / f) != frames_hash(two / f)]
    print(f"{len(files)} images, {len(diff)} differ" + (": " + " ".join(diff) if diff else ""))
    return 1 if diff else 0


class Ctx(hosttests.Ctx):
    def __init__(self, game, quick):
        super().__init__(game, quick)
        self.chdrive = self.game / "tools" / "chsim" / "chdrive.py"
        if not self.chdrive.exists():
            self.chdrive = TOOLS / "chsim" / "chdrive.py"

    def drive(self, script, outdir, env=None):
        outdir = Path(outdir)
        if outdir.exists():
            shutil.rmtree(outdir)
        r = self.run([self.chdrive, "--sim", self.game, script, outdir], env=env)
        return r.returncode == 0, r.stdout + r.stderr


def card_env(cfg, ctx, script):
    """The CHSD_CARD this script runs with, or None."""
    c = cfg.CARD
    if not c:
        return None
    which = c["scripts"]
    hit = script.stem in which if isinstance(which, (set, frozenset, list, tuple)) else script.stem.startswith(which)
    if not hit:
        return None
    f = c["file"]
    if callable(f):
        path = Path(f(ctx))
    else:
        path = ctx.game / f
        if not path.exists() and c.get("build"):
            ctx.log(f"   making {f}")
            r = ctx.run(c["build"])
            if r.returncode:
                ctx.log((r.stdout + r.stderr)[-1500:])
    return {"CHSD_CARD": str(path)}


def check(game, quick=False, no_device=False):
    game = paths.sketch(game)
    cfg = gamecfg.load(game)
    ctx = Ctx(game, quick)
    failed = []
    ctx.out.mkdir(exist_ok=True)

    for title, argv in cfg.PRE_STEPS:
        print(f"== {title}")
        r = ctx.run(argv)
        print((r.stdout + r.stderr).strip()[-1500:])
        if r.returncode:
            failed.append(title)
    if cfg.before_check:
        print("== preparation")
        if not cfg.before_check(ctx):
            failed.append("preparation")

    print("== host tests")
    r = ctx.run([TOOLS / "hosttests.py", game] + (list(cfg.QUICK_ARGS) if quick else []))
    print(r.stdout.strip()[-1500:])
    if r.returncode:
        print(r.stderr[-2000:])
        failed.append("host tests")
    if cfg.after_host_tests and not cfg.after_host_tests(ctx, r.stdout):
        failed.append("host tests (follow-up)")

    scripts = sorted((game / "tools" / "scripts").glob("*.txt"))
    print(f"== scripts ({len(scripts)})")
    if not scripts:
        print("  none")
    for script in scripts:
        name = script.stem
        if name.startswith(tuple(cfg.SKIP_SCRIPTS)):
            print(f"  skip  {name}  (device only)")
            continue
        once = name in cfg.ONCE_SCRIPTS or bool(FREE.search(script.read_text(encoding="utf-8", errors="replace")))
        env = card_env(cfg, ctx, script)
        good, text = ctx.drive(script, ctx.out / name, env)
        bugs = [ln for ln in text.splitlines() if ln.startswith("BUG")]
        line = f"  {'ok   ' if good and not bugs else 'FAIL '} {name:12s}"
        if not good or bugs:
            print(line)
            print(text[-1500:])
            failed.append(f"scripts ({name})")
            continue
        if not quick and not once:
            again = ctx.out / f"{name}.again"
            good2, _ = ctx.drive(script, again, env)
            files = sorted(p.name for p in (ctx.out / name).iterdir() if p.suffix in (".png", ".gif"))
            diff = [f for f in files if not (again / f).exists()
                    or frames_hash(ctx.out / name / f) != frames_hash(again / f)]
            shutil.rmtree(again, ignore_errors=True)
            if not good2 or diff:
                print(f"{line}  NOT DETERMINISTIC: {diff}")
                failed.append(f"scripts ({name} not deterministic)")
                continue
            line += f"  ({len(files)} images, identical twice)"
        elif once and not quick:
            line += "  (once: wall-clock)" if name not in cfg.ONCE_SCRIPTS else "  (once)"
        for ln in text.splitlines():
            if ln.startswith(ECHO_ALWAYS + tuple(cfg.ECHO)):
                line += "\n      " + ln
        print(line)

    diffs = sorted(game.glob(cfg.REDRAW["scripts"]))
    if not quick:
        print(f"== redraw ({len(diffs)} scripts)")
        if not diffs:
            print("  none")
        for script in diffs:
            for ticks in cfg.REDRAW["ticks"]:
                r = ctx.run([TOOLS / "chsim" / "diffdrive.py", game, script, ctx.out / f"{script.stem}_t{ticks}", ticks])
                tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["no output"]
                clean = r.returncode == 0 and " 0 diff episodes" in tail[0]
                print(f"  {'ok   ' if clean else 'FAIL '} {script.stem} x{ticks}: {tail[0]}")
                if not clean:
                    print((r.stdout + r.stderr)[-1500:])
                    failed.append(f"redraw ({script.stem} x{ticks})")

    if cfg.SIM_TESTS:
        print(f"== simulator tests ({len(cfg.SIM_TESTS)})")
        for script in cfg.SIM_TESTS:
            r = ctx.run([script])
            print("\n".join("  " + ln for ln in (r.stdout + r.stderr).strip().splitlines()[-12:]))
            if r.returncode:
                failed.append(f"simulator tests ({Path(script).name})")

    if not no_device:
        print("== device build (compile only)")
        r = subprocess.run([sys.executable, str(TOOLS / "device.py"), "--sketch", str(game), "build"],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        out = (r.stdout + r.stderr).strip()
        print("\n".join(out.splitlines()[-3:]))
        if r.returncode:
            failed.append("device build")
        for need in cfg.BUILD_REQUIRE:
            if need not in out:
                print(f"  FAIL: the build must print {need!r}")
                failed.append(f"device build ({need})")

    print("ALL GOOD" if not failed else "SOMETHING FAILED: " + "; ".join(failed))
    return 0 if not failed else 1


def main(game=None, argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    if game is None:
        ap.add_argument("game", help="the game's folder or name")
    ap.add_argument("--quick", action="store_true", help="the short host tests, and each script once")
    ap.add_argument("--no-device", action="store_true", help="skip the device compile")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"), help="compare two runs' images")
    a = ap.parse_args(argv)
    if a.compare:
        return compare(*a.compare)
    return check(game or a.game, a.quick, a.no_device)


if __name__ == "__main__":
    sys.exit(main())
