#!/usr/bin/env python3
"""Build out/chsaveman_card.img: MBR + one FAT32 partition (label CHSAVEMAN),
GAMES/*.CHG and GAMES/MORE/*.CHG, 8.3 names only. Pure Python, LF endings.
usage: mkcard.py [--out F] [--size-mb 64] [--max N] [--no-saveman] [folder-or-file ...]
Default sources: one dist .CHG per save code from CHSaveMan/tools/tests/scan_truth.txt.
Save pages live in flash, so nothing is seeded for them (--save-pages is accepted, no-op)."""
import argparse, os, re, struct, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOME = os.path.expanduser("~")
TRUTH = os.path.join(ROOT, "CHSaveMan", "tools", "tests", "scan_truth.txt")
SAVEMAN = os.path.join(ROOT, "dist", "SAVEMAN.CHG")
SS = 512
RSV = 32


def default_sources(n):
    seen, out, nocode = set(), [], 0
    for ln in open(TRUTH, encoding="utf-8"):
        if ln.startswith("#") or not ln.strip():
            continue
        code, path = ln.split(None, 1)
        path = os.path.join(HOME, path.strip())
        if "/dist/" not in path.replace("\\", "/") or not os.path.isfile(path):
            continue
        if code in seen:
            continue
        if code == "-":
            if nocode >= 1:
                continue
            nocode += 1
        seen.add(code)
        out.append(path)
        if len(out) >= n:
            break
    return out


def n83(name, used):
    stem, _, ext = name.upper().rpartition(".")
    stem = re.sub(r"[^A-Z0-9_]", "_", stem)[:8] or "X"
    ext = re.sub(r"[^A-Z0-9_]", "_", ext)[:3]
    cand, i = stem, 0
    while (cand, ext) in used:
        i += 1
        cand = stem[:8 - len(str(i)) - 1] + "~" + str(i)
    used.add((cand, ext))
    return cand.ljust(8) + ext.ljust(3)


class Fat:
    def __init__(self, mb):
        self.start = 2048
        self.total = mb * 2048 - self.start
        fatsz = 1
        while True:
            clus = self.total - RSV - 2 * fatsz
            need = (clus + 2) * 4 // SS + 1
            if need <= fatsz:
                break
            fatsz = need
        assert clus >= 65525
        self.fatsz, self.clus = fatsz, clus
        self.fat = [0] * (clus + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.data = {}
        self.nxt = 2

    def alloc(self, payload):
        n = max(1, -(-len(payload) // SS))
        first = self.nxt
        assert first + n <= self.clus + 2, "image too small"
        for i in range(n):
            c = first + i
            self.fat[c] = c + 1 if i < n - 1 else 0x0FFFFFFF
            self.data[c] = payload[i * SS:(i + 1) * SS].ljust(SS, b"\0")
        self.nxt += n
        return first

    @staticmethod
    def ent(name, attr, cl, size):
        return name.encode("ascii") + bytes([attr, 0, 0]) + struct.pack(
            "<HHHHHHHI", 0, 0x5A21, 0x5A21, cl >> 16, 0, 0x5A21, cl & 0xFFFF, size)

    def rewrite(self, cl, payload):
        self.data[cl] = payload[:SS].ljust(SS, b"\0")
        prev = cl
        for i in range(1, max(1, -(-len(payload) // SS))):
            c = self.nxt
            self.nxt += 1
            self.fat[prev] = c
            self.fat[c] = 0x0FFFFFFF
            self.data[c] = payload[i * SS:(i + 1) * SS].ljust(SS, b"\0")
            prev = c

    def mk(self, t, self_cl, parent_cl):
        b = b""
        if parent_cl is not None:
            b += self.ent(".".ljust(11), 0x10, self_cl, 0) + self.ent("..".ljust(11), 0x10, 0 if parent_cl == 2 else parent_cl, 0)
        for nm, v in t.items():
            if isinstance(v, dict):
                cl = self.alloc(b"")
                self.rewrite(cl, self.mk(v, cl, self_cl))
                b += self.ent(nm, 0x10, cl, 0)
            else:
                b += self.ent(nm, 0x20, self.alloc(v), len(v))
        return b

    def build(self, tree, label):
        self.alloc(b"")
        root = self.ent(label.ljust(11), 0x08, 0, 0) + self.mk(tree, 2, None)
        self.rewrite(2, root)

    def image(self, label):
        bs = bytearray(SS)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"MSDOS5.0"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SS, 1, RSV, 2, 0, 0, 0xF8, 0, 63, 255, self.start, self.total)
        struct.pack_into("<IHHIHH", bs, 36, self.fatsz, 0, 0, 2, 1, 6)
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, 0xC4A5E001)
        bs[71:82] = label.ljust(11).encode()
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        fsi = bytearray(SS)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<III", fsi, 484, 0x61417272, self.clus - (self.nxt - 2), self.nxt)
        fsi[510:512] = b"\x55\xAA"
        fat = struct.pack("<%dI" % len(self.fat), *self.fat).ljust(self.fatsz * SS, b"\0")
        mbr = bytearray(SS)
        mbr[446:462] = bytes([0x80, 0xFE, 0xFF, 0xFF, 0x0C, 0xFE, 0xFF, 0xFF]) + struct.pack("<II", self.start, self.total)
        mbr[510:512] = b"\x55\xAA"
        vol = bytearray(bs) + fsi + b"\0" * SS * 4 + bs + fsi
        vol += b"\0" * ((RSV - len(vol) // SS) * SS)
        vol += fat + fat
        buf = bytearray(self.clus * SS)
        for c, d in self.data.items():
            buf[(c - 2) * SS:(c - 1) * SS] = d
        return bytes(mbr) + b"\0" * ((self.start - 1) * SS) + bytes(vol) + bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", nargs="*")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "chsaveman_card.img"))
    ap.add_argument("--size-mb", type=int, default=64)
    ap.add_argument("--max", type=int, default=12)
    ap.add_argument("--no-saveman", action="store_true")
    ap.add_argument("--save-pages", action="store_true")
    a = ap.parse_args()
    files = []
    for s in a.src:
        if os.path.isdir(s):
            files += [os.path.join(s, f) for f in sorted(os.listdir(s)) if f.upper().endswith(".CHG")]
        else:
            files.append(s)
    if not a.src:
        files = default_sources(a.max)
    files = files[:a.max]
    used, games, more, manifest, mused = set(), {}, {}, [], set()
    for i, p in enumerate(files):
        d = open(p, "rb").read()
        if i == len(files) - 1 and len(files) > 2:
            nm = n83(os.path.basename(p), mused)
            more[nm] = d
            dn = "GAMES/MORE/"
        else:
            nm = n83(os.path.basename(p), used)
            games[nm] = d
            dn = "GAMES/"
        manifest.append((dn + nm[:8].rstrip() + "." + nm[8:].rstrip(), d, p))
    if not a.no_saveman and os.path.isfile(SAVEMAN):
        d = open(SAVEMAN, "rb").read()
        games[n83("SAVEMAN.CHG", used)] = d
        manifest.append(("GAMES/SAVEMAN.CHG", d, SAVEMAN))
    if more:
        games["MORE       "] = more
    f = Fat(a.size_mb)
    f.build({"GAMES      ": games}, "CHSAVEMAN")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "wb") as o:
        o.write(f.image("CHSAVEMAN"))
    with open(a.out + ".manifest", "w", newline="") as o:
        for n, d, p in manifest:
            o.write("%s %d %s %s\n" % (hashlib.sha256(d).hexdigest(), len(d), n, p.replace("\\", "/")))
    print("wrote", a.out, len(manifest), "files")


main()
