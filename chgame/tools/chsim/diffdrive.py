"""The redraw check: a game's incremental redraw against a full one, frame by frame.

    chgame redraw <script> <outdir> [ticks_per_render]      (from the game's folder)
    python tools/chsim/diffdrive.py GAME <script> <outdir> [ticks_per_render]

Builds the game's simulator twice: A as it is, B with the define the game's
tools/game.py names (REDRAW["define"], CHSIM_FORCE_FULL by default), which
makes its presenter redraw everything every frame. The same chdrive-style
script runs on both and the framebuffers are compared after every rendered
frame: any difference is a pixel the incremental redraw left stale or
failed to update.

ticks_per_render: 1 = a render every logic tick; 3 = the catch-up of a slow
frame (what chdrive's `wait N` does). Script ops: wait tap hold release say
snap label ticks. Prints "DIFF frames a-b [label] max N px, union box ..."
per episode and saves the first frame of each as _A/_B/_D (D = the
differing pixels in magenta), then "N renders, K diff episodes". The
environment knobs DUMPALL (every differing frame) and SNAPALL with
SNAPFROM/SNAPTO (every frame in a range) save more.
"""
import importlib.util
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import gamecfg  # noqa: E402
import paths  # noqa: E402
from chdrivelib import Driver, SimTransport, mask_of  # noqa: E402
from chsim import build  # noqa: E402
from fbimage import to_image  # noqa: E402

FB = 8192


def ident_of(game):
    """The game's handshake ident, from its tools/chsim/chdrive.py."""
    f = game / "tools" / "chsim" / "chdrive.py"
    if not f.exists():
        return ""
    spec = importlib.util.spec_from_file_location(f"chdrive_{game.name}", f)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return getattr(m, "IDENT", "")


class Pair:
    def __init__(self, exes, ident, ticks):
        self.a = Driver(SimTransport(exes[0]), ident)
        self.b = Driver(SimTransport(exes[1]), ident)
        self.ticks = ticks
        self.frame = 0
        self.episodes = []
        self.cur = None
        self.label = ""

    def both(self, fn):
        return fn(self.a), fn(self.b)

    def compare(self, outdir):
        da, db = self.a.shot(), self.b.shot()
        fa, fb = da[:FB], db[:FB]
        if fa == fb:
            if self.cur:
                self.episodes.append(self.cur)
                self.cur = None
            return
        px = []
        for i in range(FB):
            if fa[i] != fb[i]:
                y, xb = divmod(i, 64)
                if (fa[i] ^ fb[i]) & 0x0F:
                    px.append((xb * 2, y))
                if (fa[i] ^ fb[i]) & 0xF0:
                    px.append((xb * 2 + 1, y))
        xs = [p[0] for p in px]
        ys = [p[1] for p in px]
        box = (min(xs), min(ys), max(xs), max(ys))
        if not self.cur or os.environ.get("DUMPALL"):
            if not self.cur:
                self.cur = {"start": self.frame, "label": self.label, "max": 0, "boxes": []}
            name = f"f{self.frame:05d}"
            to_image(da, 3).save(outdir / f"{name}_A.png")
            to_image(db, 3).save(outdir / f"{name}_B.png")
            im = to_image(db, 1)         # B with the differing pixels in magenta
            p = im.load()
            for (x, y) in px:
                p[x, y] = (255, 0, 255)
            im.resize((384, 384)).save(outdir / f"{name}_D.png")
        self.cur["end"] = self.frame
        if len(px) > self.cur["max"]:
            self.cur["max"] = len(px)
        self.cur["boxes"].append(box)

    def step(self, n, outdir):
        while n > 0:
            if os.environ.get("SNAPALL") and \
                    int(os.environ.get("SNAPFROM", "0")) <= self.frame <= int(os.environ.get("SNAPTO", "99999")):
                to_image(self.a.shot(), 3).save(outdir / f"all{self.frame:05d}_A.png")
                to_image(self.b.shot(), 3).save(outdir / f"all{self.frame:05d}_B.png")
            k = min(self.ticks, n)
            self.a.cmd(f"N {k}")
            self.b.cmd(f"N {k}")
            n -= k
            self.frame += 1
            self.compare(outdir)

    def say(self, d, args):
        d.t.send(" ".join(args))
        for _ in range(10000):
            line = d.t.readline()
            if line.startswith("OK ") and line[3:].strip().isdigit():
                d.t.send("N 5")
                continue
            if line.startswith("OK"):
                break
            if line.startswith("ERR"):
                raise SystemExit("refused " + " ".join(args))

    def run(self, script, outdir):
        outdir = Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        for d in (self.a, self.b):
            d.handshake()
            d.cmd("L1")
        for raw in Path(script).read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            op, *args = line.split()
            if op == "label":
                self.label = " ".join(args)
            elif op == "wait":
                self.step(int(args[0]), outdir)
            elif op == "tap":
                m = mask_of(args[0])
                self.both(lambda d: d.buttons(m))
                self.step(int(args[1]) if len(args) > 1 else 3, outdir)
                self.both(lambda d: d.buttons(0))
                self.step(1, outdir)
            elif op == "hold":
                m = mask_of(args[0])
                self.both(lambda d: d.buttons(m))
            elif op == "release":
                self.both(lambda d: d.buttons(0))
            elif op == "say":
                self.say(self.a, args)
                self.say(self.b, args)
            elif op == "snap":
                to_image(self.a.shot(), 3).save(outdir / f"{args[0]}_A.png")
            elif op == "ticks":
                self.ticks = int(args[0])
            else:
                raise SystemExit("bad op " + line)
        if self.cur:
            self.episodes.append(self.cur)
        for b in self.a.t.bugs + self.b.t.bugs:
            print("SIMBUG", b)
        for e in self.episodes:
            bx = e["boxes"]
            u = (min(b[0] for b in bx), min(b[1] for b in bx), max(b[2] for b in bx), max(b[3] for b in bx))
            print(f"DIFF frames {e['start']}-{e['end']} [{e['label']}] max {e['max']} px, "
                  f"union box x{u[0]}-{u[2]} y{u[1]}-{u[3]}")
        print(f"{self.frame} renders, {len(self.episodes)} diff episodes")
        self.a.t.close()
        self.b.t.close()


def redraw(game, script, outdir, ticks=1):
    game = paths.sketch(game)
    cfg = gamecfg.load(game)
    a = build(game)
    b = build(game, [cfg.REDRAW["define"]], out=a.parent / "sim_full.exe")
    Pair((a, b), ident_of(game), ticks).run(script, outdir)
    return 0


def main(game=None, argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if game is None:
        if len(args) < 3:
            raise SystemExit(__doc__)
        game, args = args[0], args[1:]
    if len(args) < 2:
        raise SystemExit("usage: chgame redraw <script> <outdir> [ticks_per_render]")
    return redraw(game, args[0], args[1], int(args[2]) if len(args) > 2 else 1)


if __name__ == "__main__":
    sys.exit(main())
