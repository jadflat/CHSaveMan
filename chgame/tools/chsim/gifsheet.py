"""Tile a GIF's frames into one image for review.

    python tools/chsim/gifsheet.py IN.gif OUT.png [--every N] [--start F] [--count N] [--cols C] [--scale S]
"""
import argparse

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif")
    ap.add_argument("out")
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--start", type=int, default=0, help="first frame")
    ap.add_argument("--count", type=int, default=0, help="frames to show (0: to the end)")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--scale", type=float, default=0.5)
    a = ap.parse_args()
    im = Image.open(a.gif)
    frames = []
    i = 0
    try:
        while True:
            if i >= a.start and (i - a.start) % a.every == 0 and (not a.count or len(frames) < a.count):
                f = im.convert("RGB")
                frames.append(f.resize((int(f.width * a.scale), int(f.height * a.scale)), Image.NEAREST))
            i += 1
            im.seek(i)
    except EOFError:
        pass
    w, h = frames[0].size
    rows = (len(frames) + a.cols - 1) // a.cols
    sheet = Image.new("RGB", (a.cols * (w + 2), rows * (h + 2)), (40, 40, 40))
    for k, f in enumerate(frames):
        sheet.paste(f, ((k % a.cols) * (w + 2), (k // a.cols) * (h + 2)))
    sheet.save(a.out)
    print(f"{len(frames)} frames -> {a.out}")


if __name__ == "__main__":
    main()
