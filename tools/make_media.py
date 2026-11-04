"""SaveMan's artwork from STARLIFT's sprites (HOUSE palette), written to docs/:
  cart.png      128x128 menu picture (title, scene)
  backdrop.png  128x128 scene with no text (for the app or a wallpaper)
  banner.png    1280x640 README header (the scene stretched to 2:1, 5x)
  lift.gif      the lander lifting a save disk, 128x128 x 5
Needs the STARLIFT sprite PNGs (CHStarlift/out/sprites; run its tools/make_sprites.py) next to this repo's parent."""
import os, random, sys
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SPR = os.environ.get('STARLIFT_SPRITES') or os.path.join(ROOT, '..', 'CHStarlift', 'out', 'sprites')
sys.path.insert(0, os.path.join(ROOT, '..', 'CHStarlift', 'tools'))
HOUSE = ['000000', 'FFFFFF', '146b34', '2fb34a', '7be26a', 'BBBBCC', 'EE1122', '770022', 'FFCC22', '774411', '2266EE', '112255', 'FFBB88', '66EEFF']
KEY = {'k': 0, 'w': 1, 'G': 2, 'g': 3, 'l': 4, 's': 5, 'r': 6, 'm': 7, 'y': 8, 'b': 9, 'u': 10, 'n': 11, 'o': 12, 'c': 13}
def rgb(i): h = HOUSE[i]; return (int(h[:2], 16), int(h[2:4], 16), int(h[4:], 16), 255)
def spr(n): return Image.open(os.path.join(SPR, n + '.png')).convert('RGBA')
def put(im, s, x, y, flip=False):
    if flip: s = s.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    im.alpha_composite(s, (int(x), int(y)))

def sky(w, h, hor):
    """Deep blue at the top, dithering down to bright blue, then cyan haze at the horizon."""
    im = Image.new('RGBA', (w, h)); px = im.load()
    for y in range(h):
        t = y / hor
        for x in range(w):
            d = (x + y) % 2 == 0
            if t < .35: c = 11
            elif t < .6: c = 10 if d else 11
            elif t < .85: c = 10
            else: c = 13 if (d and t > .92) else 10
            px[x, y] = rgb(c)
    return im

def scene(w, h, floppy_y, logo):
    rnd = random.Random(7)
    hor = int(h * .70)
    im = sky(w, h, hor)
    for _ in range(w * h // 90):
        x, y = rnd.randrange(w), rnd.randrange(int(hor * .6))
        im.putpixel((x, y), rgb(1 if rnd.random() < .3 else 5))
    put(im, spr('SKY_RINGED'), w * .62, h * .10)
    put(im, spr('SKY_EARTH'), w * .08, h * .30)
    put(im, spr('SAUCER0'), w * .80, h * .45); put(im, spr('SAUCER2'), w * .14, h * .12)
    # hills
    px = im.load()
    for x in range(w):
        top = hor + int(3 * ((x * 7 % 23) / 23)) - 2 + (3 if (x // 9) % 2 else 0)
        for y in range(top, h):
            c = 3 if y == top else (2 if (y - top) % 5 else 4) if (y - top) < 3 else 2
            if y > top + 2: c = 2 if (x + y) % 3 else 3 if False else 2
            px[x, y] = rgb(c if y > top else 4)
    gy = hor
    put(im, spr('BASE'), w * .66, gy - 24)
    for n, fx, fy, fl in (('PL_FERN0', .06, 6, 0), ('PL_MUSH0', .50, 10, 0), ('PL_FERN1', .93, 5, 1), ('PL_FLOWER0', .40, 8, 0)):
        s = spr(n); put(im, s, w * fx, gy + fy, fl)
    for i, x in enumerate((.22, .28, .55)):
        put(im, spr('MAN_CHEER' if i != 1 else 'MAN_WAVE1'), w * x, gy + 5 + i % 2 * 4)
    for n, fx, fy, fl in (('COW', .08, 14, 0), ('COWEAT', .82, 17, 1), ('COW', .70, 24, 1)):
        put(im, spr(n), w * fx, gy + fy, fl)
    put(im, spr('COWUP'), w * .30, gy - 30)           # one cow already on its way up
    # beam, lander, disk
    lx, ly = w * .40, h * .28
    beam = Image.new('RGBA', (w, h)); bp = beam.load()
    for y in range(int(ly) + 20, gy + 8):
        half = 3 + (y - int(ly) - 20) // 3
        for x in range(int(lx) + 11 - half, int(lx) + 12 + half):
            if (x + y) % 2 == 0: bp[x, y] = rgb(13)
    im.alpha_composite(beam)
    put(im, spr('LAND3'), lx, ly)
    put_floppy(im, int(lx) + 11, int(floppy_y))
    if logo:
        import life
        life.FONT.setdefault('N', ['#...#', '##..#', '##..#', '#.#.#', '#..##', '#..##', '#...#'])
        title_logo = life.title_logo
        rows = title_logo('SAVEMAN'); lg = Image.new('RGBA', (len(rows[0]), len(rows)))
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                if ch != '.': lg.putpixel((x, y), rgb(KEY[ch]))
        put(im, lg, (w - lg.width) // 2, 1)
    return im

def put_floppy(im, cx, y):
    d = Image.new('RGBA', (14, 14))
    for yy in range(14):
        for xx in range(14):
            c = None
            if xx in (0, 13) or yy in (0, 13): c = 0
            elif yy < 5 and 3 <= xx <= 9: c = 5
            elif yy > 7 and 2 <= xx <= 11: c = 1
            elif yy > 7 and xx in (2, 11): c = 1
            else: c = 10
            d.putpixel((xx, yy), rgb(c))
    d.putpixel((11, 1), rgb(0)); d.putpixel((10, 11), rgb(6)); d.putpixel((9, 11), rgb(6)); d.putpixel((8, 11), rgb(6))
    put(im, d, cx - 7, y)

out = os.path.join(ROOT, 'docs'); os.makedirs(out, exist_ok=True)
def eleven(im):
    """The menu picture may use 11 colours: fold the three least used HOUSE colours into near ones."""
    fold = {rgb(7)[:3]: rgb(6)[:3], rgb(9)[:3]: rgb(8)[:3], rgb(12)[:3]: rgb(1)[:3]}
    im = im.convert('RGB'); im.putdata([fold.get(c, c) for c in im.getdata()])
    return im
cart = eleven(scene(128, 128, 56, True)); cart.save(os.path.join(out, 'cart.png'))
scene(128, 128, 56, False).convert('RGB').save(os.path.join(out, 'backdrop.png'))
wide = scene(256, 128, 56, True); wide.convert('RGB').resize((1280, 640), Image.Resampling.NEAREST).save(os.path.join(out, 'banner.png'))
fr = [scene(128, 128, 90 - i * 7, False).convert('RGB').resize((256, 256), Image.Resampling.NEAREST) for i in range(10)]
fr = fr + fr[::-1]
fr[0].save(os.path.join(out, 'lift.gif'), save_all=True, append_images=fr[1:], duration=90, loop=0)
print('ok')
