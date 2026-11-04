"""CHSaveMan's box art for the CHGame menu: a floppy disk, 128x128, 9 colours (HOUSE palette).
Run: python CHSaveMan/tools/make_boxart.py  ->  docs/cart.png
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'docs', 'cart.png')
K, W, S, R, G, B, N, C = '#000000', '#FFFFFF', '#BBBBCC', '#EE1122', '#FFCC22', '#2266EE', '#112255', '#66EEFF'
im = Image.new('RGB', (128, 128), N)
d = ImageDraw.Draw(im)
for y in range(0, 128, 8):                       # faint grid backdrop
    d.line([(0, y), (127, y)], fill=K)
d.rectangle([14, 12, 113, 113], fill=K)           # outline
d.polygon([(16, 13), (96, 13), (112, 29), (112, 112), (16, 112)], fill=B)   # body, cut corner
d.polygon([(96, 13), (112, 29), (96, 29)], fill=C)                           # corner highlight
d.line([(16, 13), (96, 13)], fill=C)
d.line([(16, 13), (16, 112)], fill=C)
d.rectangle([34, 14, 90, 46], fill=S)             # metal shutter
d.rectangle([38, 14, 90, 46], outline=K)
d.rectangle([74, 20, 84, 40], fill=K)             # shutter slot
d.rectangle([76, 22, 82, 38], fill=N)
d.rectangle([28, 62, 100, 111], fill=W)           # label
d.rectangle([28, 62, 100, 111], outline=K)
for y in (72, 82, 92):
    d.line([(34, y), (94, y)], fill=C)
d.rectangle([34, 98, 60, 104], fill=R)            # red stripe on label
d.text((66, 97), "SAVE", fill=K)
d.rectangle([20, 104, 26, 110], fill=K)           # write-protect notch
os.makedirs(os.path.dirname(OUT), exist_ok=True)
im.save(OUT)
print(len(set(im.getdata())), "colours")
