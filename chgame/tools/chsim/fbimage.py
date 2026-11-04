"""Turn CHGfx framebuffer dumps into images.

A dump is 8192 bytes of 4 bpp framebuffer (2 px/byte, even x in the low
nibble, 64 bytes per row) followed by 16 RGB565 palette entries (32 bytes,
little endian) - what the device's `S` command and the simulator write.
Colours are shown as the 12 bpp panel shows them (RGB444).
"""
import struct
import sys

from PIL import Image

W = H = 128
FB_BYTES = W * H // 2


def palette_rgb(pal_bytes):
    out = []
    for i in range(16):
        (v,) = struct.unpack_from("<H", pal_bytes, i * 2)
        r5, g6, b5 = v >> 11, (v >> 5) & 63, v & 31
        r4, g4, b4 = r5 >> 1, g6 >> 2, b5 >> 1        # what 12 bpp keeps
        out.append((r4 * 17, g4 * 17, b4 * 17))
    return out


def to_image(dump, scale=3):
    fb, pal = dump[:FB_BYTES], dump[FB_BYTES:FB_BYTES + 32]
    rgb = palette_rgb(pal)
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        row = fb[y * 64:(y + 1) * 64]
        for xb in range(64):
            b = row[xb]
            px[xb * 2, y] = rgb[b & 15]
            px[xb * 2 + 1, y] = rgb[b >> 4]
    if scale != 1:
        im = im.resize((W * scale, H * scale), Image.NEAREST)
    return im


def save_gif(frames, path, duration):
    """Frames (RGB images) to an animated GIF that every viewer shows right.

    Each frame is stored whole, on one palette shared by the animation (the
    game shows 16 colours at a time; fades and palette cycling add more over
    a clip, never past 256). Left to its defaults, Pillow stores only the box
    that changed since the previous frame, with the unchanged pixels inside
    it made transparent: correct to the letter, but a viewer that drops or
    mis-composites a frame leaves the parts that rarely change - the HUD
    above all - showing stale pictures. disposal=2 with no transparent
    colour makes Pillow write full frames. Identical frames still merge
    into one longer one.
    """
    colours = {}
    for f in frames:
        for _, c in f.convert("RGB").getcolors(1 << 16):
            colours.setdefault(c, len(colours))
    if len(colours) > 256:
        raise ValueError(f"{len(colours)} colours: too many for one GIF palette")
    flat = [v for c in sorted(colours, key=colours.get) for v in c]
    pal = Image.new("P", (1, 1))
    pal.putpalette(flat + [0] * (768 - len(flat)))
    # Exact colours, so quantize() is only a lookup: no dithering needed.
    out = [f.convert("RGB").quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    out[0].save(path, save_all=True, append_images=out[1:], duration=duration, loop=0,
                optimize=False, disposal=2)


def sheet(images, cols=4, pad=6, labels=None):
    if not images:
        return None
    w, h = images[0].size
    rows = (len(images) + cols - 1) // cols
    out = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad) + pad), (40, 40, 40))
    for i, im in enumerate(images):
        out.paste(im, (pad + (i % cols) * (w + pad), pad + (i // cols) * (h + pad)))
    return out


if __name__ == "__main__":
    for path in sys.argv[1:]:
        with open(path, "rb") as f:
            to_image(f.read()).save(path.rsplit(".", 1)[0] + ".png")
