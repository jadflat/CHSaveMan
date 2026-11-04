"""Make a game's one README picture: docs/gameplay.gif.

    python tools/readme_gif.py CHFour            (from the repository root)
    chgame gif                                   (from the game's folder)
    python tools/readme_gif.py CHFour --no-run   join what out/gameplay holds
    python tools/readme_gif.py --check           every game's GIF against the rules

Every game's README shows one GIF: the title screen, then several short
clips of play, no more than 1 MB (docs/game-readme.md has the README's
format). The game's tools/scripts/gameplay.txt records the clips, each with
its own `rec start` / `rec stop NAME`, named so that they sort in the order
they are shown: 01_title, 02_deal, 03_win ... This tool

  1. runs that script in the simulator (into out/gameplay/),
  2. joins out/gameplay/[0-9][0-9]_*.gif in name order, holding each clip's
     last frame a little longer so the cut reads as a cut,
  3. writes docs/gameplay.gif and refuses if it is over the limit.

Over the limit: record less (shorter clips, or `rec start 4` instead of 3
for fewer frames a second) rather than more clips cut finer. --every N
keeps only every N-th frame of what was recorded, as a quick way to see
what a lower frame rate would cost and look like.

The simulator is deterministic, so the same script gives the same GIF.
"""
import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import paths  # noqa: E402
LIMIT = 1_000_000          # bytes: "no more than 1 MB", with room for either megabyte
HOLD_MS = 700              # extra time on each clip's last frame


def clip_frames(path, every=1):
    """A GIF's frames as RGB images with their durations (ms)."""
    im = Image.open(path)
    frames, carry = [], 0
    for i in range(im.n_frames):
        im.seek(i)
        d = im.info.get("duration", 50) + carry
        if i % every == 0:
            frames.append([im.convert("RGB"), d])
            carry = 0
        else:
            frames[-1][1] += d
    return frames


def join(clips, out, every=1, hold=HOLD_MS):
    frames = []
    for c in clips:
        f = clip_frames(c, every)
        f[-1][1] += hold
        frames += f
    # Identical neighbours become one longer frame.
    merged = [frames[0]]
    for im, d in frames[1:]:
        if im.tobytes() == merged[-1][0].tobytes():
            merged[-1][1] += d
        else:
            merged.append([im, d])
    # Frames go to Pillow as RGB, as chdrive's own recordings do: each gets
    # a palette of just the colours it shows (16 at a time on this screen),
    # which packs far smaller than one palette for the whole reel.
    imgs = [im for im, _ in merged]
    colours = max(len(im.getcolors(1 << 16)) for im in imgs)
    out.parent.mkdir(parents=True, exist_ok=True)
    imgs[0].save(out, save_all=True, append_images=imgs[1:],
                 duration=[d for _, d in merged], loop=0)
    return len(merged), sum(d for _, d in merged) / 1000.0, colours


def check():
    bad = 0
    for game in paths.games():
        docs = game / "docs"
        gifs = sorted(p.name for p in docs.glob("*.gif")) if docs.is_dir() else []
        g = docs / "gameplay.gif"
        notes = []
        if gifs != ["gameplay.gif"]:
            notes.append(f"docs/ holds {gifs or 'no GIF'}")
        if g.exists():
            size = g.stat().st_size
            if size > LIMIT:
                notes.append("over the limit")
            im = Image.open(g)
            print(f"{game.name:14} {size:9,} B  {im.n_frames:4} frames  {im.size[0]}x{im.size[1]}"
                  + ("   " + "; ".join(notes) if notes else ""))
        else:
            print(f"{game.name:14} " + "; ".join(notes))
        bad += bool(notes)
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sketch", nargs="?", default=".", help="the game's folder or name (default: here)")
    ap.add_argument("--no-run", action="store_true", help="don't run the script: join out/gameplay as it is")
    ap.add_argument("--every", type=int, default=1, help="keep every N-th recorded frame")
    ap.add_argument("--hold", type=int, default=HOLD_MS, help="ms added to each clip's last frame")
    ap.add_argument("--check", action="store_true", help="check every game's docs/gameplay.gif")
    a = ap.parse_args(argv)
    if a.check:
        return check()

    sketch = paths.sketch(a.sketch)
    work = sketch / "out" / "gameplay"
    if not a.no_run:
        for old in work.glob("[0-9][0-9]_*.gif"):
            old.unlink()
        r = subprocess.run([sys.executable, str(sketch / "tools" / "chsim" / "chdrive.py"), "--sim",
                            str(sketch), str(sketch / "tools" / "scripts" / "gameplay.txt"), str(work)],
                           cwd=sketch)
        if r.returncode:
            raise SystemExit(f"gameplay.txt failed (exit {r.returncode})")
    clips = sorted(work.glob("[0-9][0-9]_*.gif"))
    if len(clips) < 3:
        raise SystemExit(f"{work}: {len(clips)} clips named NN_name.gif; a reel is the title and several clips")
    out = sketch / "docs" / "gameplay.gif"
    made = work / "gameplay.gif"            # docs/ changes only when it fits
    n, secs, colours = join(clips, made, a.every, a.hold)
    size = made.stat().st_size
    for c in clips:
        print(f"  {c.name:24} {Image.open(c).n_frames:4} frames  {c.stat().st_size:9,} B")
    print(f"{out.relative_to(sketch)}: {len(clips)} clips, {n} frames, {secs:.1f} s, "
          f"{colours} colours a frame at most, {size:,} B of {LIMIT:,}")
    if size > LIMIT:
        raise SystemExit(f"over the limit by {size - LIMIT:,} B: record less (see this file's header); "
                         f"docs/gameplay.gif is unchanged, the reel is {made}")
    out.parent.mkdir(parents=True, exist_ok=True)
    made.replace(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
