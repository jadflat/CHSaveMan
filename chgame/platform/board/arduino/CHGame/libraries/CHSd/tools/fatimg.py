"""FAT16/FAT32 disk images for tests and the simulators, and a small FAT reader
(CHSd; from HypeRunner's tools/hypelib, MIT).

    python tools/fatimg.py build OUT.img SRC[=NAME] ... [--fs fat32|fat16]
                 [--superfloppy] [--spc N] [--clusters N] [--fragment NAME=PIECES]
                 [--seed S] [--lfn] [--decoys]
    python tools/fatimg.py runs IMAGE PATH [PATH ...]
    python tools/fatimg.py ls IMAGE [DIR]

build_image() writes a whole card image: an MBR with one partition (or a
"superfloppy" volume at LBA 0), a boot sector, FSInfo and backup boot sector
on FAT32, both FAT copies, the directories and the files. Files may be
split into scattered pieces (fragment=) so that the device's run-list code
sees real fragmentation, and directories can be padded to span clusters.
Every byte of the image is written; nothing is sparse.

FatVolume reads an image the way src/sd/Fat.cpp reads a card (same
partition choice, same error codes; its boot sector checks are a stricter
superset, so an image it accepts mounts on the device). Tests compare the
C code against the Layout that build_image() returns, which records where
every cluster went (the ground truth).

Written from the Microsoft FAT specification (BPB, FAT type by cluster
count, directory and long-name entries, FSInfo) and the MBR layout.
"""
import argparse
import random
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

SECTOR = 512

# Error codes shared with src/sd/Fat.h.
E_NOFS, E_EXFAT, E_NOTFOUND, E_FRAG, E_CHAIN = -10, -11, -12, -13, -14
ERR_NAMES = {E_NOFS: "E_NOFS", E_EXFAT: "E_EXFAT", E_NOTFOUND: "E_NOTFOUND",
             E_FRAG: "E_FRAG", E_CHAIN: "E_CHAIN"}

FAT_TYPES = {0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E}
ATTR_RO, ATTR_HIDDEN, ATTR_SYS, ATTR_LABEL, ATTR_DIR, ATTR_ARCH = 1, 2, 4, 8, 0x10, 0x20
ATTR_LFN = 0x0F
DATE = ((2026 - 1980) << 9) | (9 << 5) | 29            # fixed, so images are reproducible
TIME = (12 << 11) | (0 << 5)


class FatError(Exception):
    def __init__(self, code, msg):
        super().__init__(f"{ERR_NAMES.get(code, code)}: {msg}")
        self.code = code


def short_name(name):
    """'hype.pak' -> b'HYPE    PAK' (8.3 only; raises on anything else)."""
    base, dot, ext = name.upper().partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
    if not base or len(base) > 8 or len(ext) > 3 or "." in ext or not set(base + ext) <= ok:
        raise ValueError(f"not an 8.3 name: {name!r}")
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def lfn_checksum(sfn):
    s = 0
    for c in sfn:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(long_name, sfn):
    """Long-name entries for long_name, in on-disk order (last part first)."""
    chars = [ord(c) for c in long_name]
    if len(chars) % 13:
        chars += [0] + [0xFFFF] * (12 - len(chars) % 13)
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    ck = lfn_checksum(sfn)
    out = []
    for i, p in enumerate(parts):
        e = bytearray(32)
        e[0] = (i + 1) | (0x40 if i == len(parts) - 1 else 0)
        struct.pack_into("<5H", e, 1, *p[0:5])
        e[11] = ATTR_LFN
        e[13] = ck
        struct.pack_into("<6H", e, 14, *p[5:11])
        struct.pack_into("<2H", e, 28, *p[11:13])
        out.append(bytes(e))
    return out[::-1]


def dir_entry(sfn, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = sfn
    e[11] = attr
    struct.pack_into("<HHHHHHHI", e, 14, TIME, DATE, DATE, cluster >> 16, TIME, DATE,
                     cluster & 0xFFFF, size)
    return e


@dataclass
class Obj:
    path: str                    # "HYPE.PAK", "HYPE", "HYPE/SAVE.DAT"; "" = root
    is_dir: bool
    data: bytes = b""
    pieces: int = 1
    entries: list = field(default_factory=list)   # directory: bytearrays, cluster fixed later
    children: list = field(default_factory=list)
    clusters: list = field(default_factory=list)

    @property
    def size(self):
        return len(self.data)


@dataclass
class Layout:
    """Where build_image() put everything (absolute LBAs)."""
    fs: str
    part_lba: int
    spc: int
    rsvd: int
    nfats: int
    fat_sectors: int
    root_secs: int
    clusters: int                # data clusters (valid numbers 2 .. clusters + 1)
    vol_sectors: int
    image_sectors: int
    root_cluster: int
    objects: dict                # path -> Obj

    @property
    def fat_lba(self):
        return self.part_lba + self.rsvd

    @property
    def root_lba(self):          # FAT16 fixed root region
        return self.fat_lba + self.nfats * self.fat_sectors

    @property
    def data_lba(self):
        return self.root_lba + self.root_secs

    def cluster_lba(self, c):
        return self.data_lba + (c - 2) * self.spc

    def runs(self, path):
        """Ground-truth extents of a file, trimmed to its size (as fat::runs)."""
        o = self.objects[path.upper()]
        left = -(-o.size // SECTOR)
        out = []
        for c in o.clusters:
            if not left:
                break
            lba, k = self.cluster_lba(c), min(left, self.spc)
            if out and out[-1][0] + out[-1][1] == lba:
                out[-1][1] += k
            else:
                out.append([lba, k])
            left -= k
        return [tuple(r) for r in out]


class Alloc:
    def __init__(self, clusters, rng):
        self.used = bytearray(clusters + 2)
        self.used[0] = self.used[1] = 1
        self.rng = rng

    def _free_runs(self):
        runs, start = [], None
        for i, u in enumerate(self.used):
            if not u and start is None:
                start = i
            elif u and start is not None:
                runs.append((start, i - start))
                start = None
        if start is not None:
            runs.append((start, len(self.used) - start))
        return runs

    def contiguous(self, n):
        for a, length in self._free_runs():
            if length >= n:
                return self._take(a, n)
        raise ValueError(f"image full: no run of {n} free clusters")

    def scattered(self, n, pieces):
        """n clusters in `pieces` runs, each surrounded by free clusters so no
        two pieces merge, placed at random (so chain order != disk order)."""
        pieces = max(1, min(pieces, n))
        sizes = [n // pieces + (1 if i < n % pieces else 0) for i in range(pieces)]
        out = []
        for s in sizes:
            opts = [(a + 1, length - s - 1) for a, length in self._free_runs() if length - s - 1 >= 1]
            total = sum(k for _, k in opts)
            if not total:
                raise ValueError(f"image too small to scatter {n} clusters into {pieces} pieces")
            pick = self.rng.randrange(total)
            for a, k in opts:
                if pick < k:
                    out += self._take(a + pick, s)
                    break
                pick -= k
        return out

    def _take(self, a, n):
        for i in range(a, a + n):
            assert not self.used[i]
            self.used[i] = 1
        return list(range(a, a + n))


def _geometry(fs, spc, clusters, root_entries, unchecked):
    if fs not in ("fat16", "fat32"):
        raise ValueError("fs must be fat16 or fat32")
    if spc not in (1, 2, 4, 8, 16, 32, 64, 128):
        raise ValueError("sectors per cluster must be a power of two <= 128")
    if not unchecked:
        if fs == "fat16" and not 4085 <= clusters <= 65524:
            raise ValueError(f"FAT16 needs 4085..65524 clusters, not {clusters} (change --spc)")
        if fs == "fat32" and clusters < 65525:
            raise ValueError(f"FAT32 needs >= 65525 clusters, not {clusters}")
    rsvd = 1 if fs == "fat16" else 32
    root_secs = -(-root_entries * 32 // SECTOR) if fs == "fat16" else 0
    fat_secs = -(-(clusters + 2) * (2 if fs == "fat16" else 4) // SECTOR)
    return rsvd, root_secs, fat_secs


def build_image(path, files, fs="fat32", mbr=True, part_lba=2048, part_type=None, part_slot=0,
                other_parts=(), spc=None, clusters=None, fragment=None, dir_pieces=None, seed=1, lfn=False,
                decoys=False, padding=None, root_entries=512, nfats=2, unchecked=False,
                label="HYPERUNNER", ext_flags=0):
    """Write a FAT image at `path` holding `files` and return its Layout.

    files      {"HYPE.PAK": bytes, "HYPE/SAVE.DAT": bytes, ...}; a name ending
               in "/" (value None) makes an empty directory. Names are 8.3;
               lower case is folded (and kept as a long name when lfn=True).
    fragment   {name: pieces} or an int for every file: scatter the file's
               clusters into that many separate runs.
    dir_pieces {dir: pieces} the same for directories ("" = FAT32 root).
    padding    {dir: n} n empty dummy files placed before a directory's real
               entries, so a lookup must walk more of the directory.
    decoys     add a deleted entry, a fake long-name entry and a volume label
               that spell a real file's short name; the reader must skip them.
    unchecked  allow a cluster count outside the FAT type's range (for the
               FAT12 refusal test).
    other_parts [(slot, type, lba, sectors)] extra MBR entries (not formatted).
    ext_flags  FAT32 BPB_ExtFlags (0x80 | n: mirroring off, only FAT n is live).
    """
    rng = random.Random(seed)
    spc = spc or (4 if fs == "fat16" else 1)
    cb = spc * SECTOR
    fragment = fragment if fragment is not None else {}
    dir_pieces = dir_pieces or {}
    padding = padding or {}

    # Objects: the root, directories, files.
    objs = {"": Obj("", True)}
    long_names = {}
    for name, data in files.items():
        parts = [p for p in name.split("/") if p]
        for i in range(len(parts)):
            key = "/".join(p.upper() for p in parts[:i + 1])
            is_dir = i < len(parts) - 1 or name.endswith("/")
            long_names[key] = parts[i]
            if key in objs:
                if objs[key].is_dir != is_dir:
                    raise ValueError(f"{key} is both a file and a directory")
                continue
            short_name(parts[i])
            o = Obj(key, is_dir, b"" if is_dir else bytes(data))
            if not is_dir:
                o.pieces = fragment if isinstance(fragment, int) else fragment.get(key, fragment.get(name, 1))
            else:
                o.pieces = dir_pieces.get(key, 1)
            objs[key] = o
            objs["/".join(p.upper() for p in parts[:i])].children.append(o)
    if fs == "fat32":
        objs[""].pieces = dir_pieces.get("", 1)

    # Directory entries (cluster numbers are patched in after allocation).
    def entries_for(d):
        ents = []
        if d.path:
            ents.append(("dot", dir_entry(b".          ", ATTR_DIR, 0, 0)))
            ents.append(("dotdot", dir_entry(b"..         ", ATTR_DIR, 0, 0)))
        else:
            ents.append((None, dir_entry(label.upper().encode("ascii")[:11].ljust(11), ATTR_LABEL, 0, 0)))
        for i in range(padding.get(d.path, 0)):
            sfn = short_name(f"PAD{i:05d}.TXT")
            if lfn:
                ents += [(None, e) for e in lfn_entries(f"Padding file {i:05d}.txt", sfn)]
            ents.append((None, dir_entry(sfn, ATTR_ARCH, 0, 0)))
        for c in d.children:
            sfn = short_name(c.path.split("/")[-1])
            if decoys:
                gone = bytearray(dir_entry(sfn, ATTR_ARCH, 0x0FFFFFF0, 12345))
                gone[0] = 0xE5
                fake_lfn = bytearray(dir_entry(sfn, ATTR_LFN, 0x0FFFFFF1, 777))
                ents += [(None, gone), (None, fake_lfn)]
                if not d.path:
                    ents.append((None, dir_entry(sfn, ATTR_LABEL, 0x0FFFFFF2, 4242)))
            if lfn:
                ents += [(None, e) for e in lfn_entries(long_names[c.path], sfn)]
            ents.append((c, dir_entry(sfn, ATTR_DIR if c.is_dir else ATTR_ARCH, 0, c.size)))
        return ents

    for o in objs.values():
        if o.is_dir:
            o.entries = entries_for(o)

    # Geometry: enough clusters for everything, with room to scatter.
    def need(o):
        if o.is_dir:
            if not o.path and fs == "fat16":
                return 0
            return max(1, -(-(len(o.entries) + 1) * 32 // cb))
        return -(-o.size // cb)
    total_need = sum(need(o) + 2 * o.pieces for o in objs.values())
    if clusters is None:
        clusters = max(total_need * 3 // 2 + 64, 4200 if fs == "fat16" else 65600)
        if fs == "fat16" and clusters > 65524:
            raise ValueError(f"{clusters} clusters needed: use a bigger --spc or fat32")
    rsvd, root_secs, fat_secs = _geometry(fs, spc, clusters, root_entries, unchecked)
    if fs == "fat16" and len(objs[""].entries) + 1 > root_entries:
        raise ValueError("too many root entries for the FAT16 root region")
    vol = rsvd + nfats * fat_secs + root_secs + clusters * spc
    base = part_lba if mbr else 0
    lay = Layout(fs, base, spc, rsvd, nfats, fat_secs, root_secs, clusters, vol, base + vol, 0, objs)

    # Allocation: the FAT32 root first (cluster 2), then directories, then files.
    al = Alloc(clusters, rng)
    order = ([objs[""]] if fs == "fat32" else []) + \
            [o for o in objs.values() if o.is_dir and o.path] + [o for o in objs.values() if not o.is_dir]
    for o in order:
        n = need(o)
        if n:
            o.clusters = al.contiguous(n) if o.pieces <= 1 else al.scattered(n, o.pieces)
    lay.root_cluster = objs[""].clusters[0] if fs == "fat32" else 0

    img = bytearray(lay.image_sectors * SECTOR)

    # MBR.
    if mbr:
        if part_type is None:
            part_type = 0x0C if fs == "fat32" else 0x06
        pe = 446 + 16 * part_slot
        img[pe:pe + 16] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", part_type, b"\xfe\xff\xff",
                                      base, vol)
        for slot, t, lba, n in other_parts:
            pe = 446 + 16 * slot
            img[pe:pe + 16] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", t, b"\xfe\xff\xff", lba, n)
        img[510:512] = b"\x55\xaa"

    # Boot sector.
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x58\x90" if fs == "fat32" else b"\xeb\x3c\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, spc, rsvd, nfats,
                     root_entries if fs == "fat16" else 0, vol if vol < 0x10000 else 0, 0xF8,
                     fat_secs if fs == "fat16" else 0, 63, 255, base, vol if vol >= 0x10000 else 0)
    vol_label = label.upper().encode("ascii")[:11].ljust(11)
    if fs == "fat16":
        struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29, 0x48595045, vol_label, b"FAT16   ")
    else:
        struct.pack_into("<IHHIHH12sBBBI11s8s", bs, 36, fat_secs, ext_flags, 0, lay.root_cluster, 1, 6,
                         bytes(12), 0x80, 0, 0x29, 0x48595045, vol_label, b"FAT32   ")
    bs[510:512] = b"\x55\xaa"
    o0 = base * SECTOR
    img[o0:o0 + SECTOR] = bs
    if fs == "fat32":
        fsi = bytearray(SECTOR)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<IIII", fsi, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF, 0)
        struct.pack_into("<I", fsi, 508, 0xAA550000)
        img[o0 + SECTOR:o0 + 2 * SECTOR] = fsi
        img[o0 + 6 * SECTOR:o0 + 7 * SECTOR] = bs
        img[o0 + 7 * SECTOR:o0 + 8 * SECTOR] = fsi

    # FAT.
    width = 2 if fs == "fat16" else 4
    eoc = 0xFFFF if fs == "fat16" else 0x0FFFFFFF
    fat = bytearray(fat_secs * SECTOR)
    fmt = "<H" if width == 2 else "<I"
    struct.pack_into(fmt, fat, 0, 0xFFF8 if width == 2 else 0x0FFFFFF8)
    struct.pack_into(fmt, fat, width, eoc)
    for o in objs.values():
        for a, b in zip(o.clusters, o.clusters[1:] + [eoc]):
            struct.pack_into(fmt, fat, a * width, b)
    for i in range(nfats):
        f0 = (lay.fat_lba + i * fat_secs) * SECTOR
        img[f0:f0 + len(fat)] = fat

    # Directories, now that every object has clusters.
    for d in objs.values():
        if not d.is_dir:
            continue
        raw = bytearray()
        for who, e in d.entries:
            e = bytearray(e)
            c = None
            if who == "dot":
                c = d.clusters[0]
            elif who == "dotdot":
                parent = d.path.rpartition("/")[0]
                c = objs[parent].clusters[0] if parent else 0          # the root is always 0 here
            elif isinstance(who, Obj):
                c = who.clusters[0] if who.clusters else 0
            if c is not None:
                struct.pack_into("<H", e, 20, c >> 16)
                struct.pack_into("<H", e, 26, c & 0xFFFF)
            raw += e
        if not d.path and fs == "fat16":
            r0 = lay.root_lba * SECTOR
            img[r0:r0 + len(raw)] = raw
        else:
            _scatter(img, lay, d.clusters, bytes(raw))

    for o in objs.values():
        if not o.is_dir:
            _scatter(img, lay, o.clusters, o.data)

    with open(path, "wb") as f:
        f.write(img)
    return lay


def _scatter(img, lay, clusters, data):
    cb = lay.spc * SECTOR
    for i, c in enumerate(clusters):
        chunk = data[i * cb:(i + 1) * cb]
        if chunk:
            o = lay.cluster_lba(c) * SECTOR
            img[o:o + len(chunk)] = chunk


def set_fat_entry(path, lay, cluster, value, copies=None):
    """Overwrite one FAT entry in every FAT copy, or in the given copies (for
    corruption tests)."""
    width = 2 if lay.fs == "fat16" else 4
    with open(path, "r+b") as f:
        for i in (range(lay.nfats) if copies is None else copies):
            f.seek((lay.fat_lba + i * lay.fat_sectors) * SECTOR + cluster * width)
            f.write(struct.pack("<H" if width == 2 else "<I", value))


def make_exfat_stub(path, mbr=True):
    """A card image whose volume is exFAT (only the boot sector's signature
    is real): the device must refuse it with E_EXFAT."""
    base = 2048 if mbr else 0
    img = bytearray((base + 64) * SECTOR)
    if mbr:
        img[446:462] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x07, b"\xfe\xff\xff", base, 64)
        img[510:512] = b"\x55\xaa"
    o = base * SECTOR
    img[o:o + 11] = b"\xeb\x76\x90EXFAT   "
    img[o + 510:o + 512] = b"\x55\xaa"
    Path(path).write_bytes(img)


# ---- reader ---------------------------------------------------------------------------

class FatVolume:
    """Read-only FAT16/32 over an image file, with src/sd/Fat.cpp's rules."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.sectors = self.f.tell() // SECTOR
        self._mount()

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def sector(self, lba):
        if lba >= self.sectors:
            raise FatError(-5, f"read past the end of the image (LBA {lba})")
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def _mount(self):
        base = 0
        for pas in (0, 1):
            b = self.sector(base)
            if b[510:512] != b"\x55\xaa":
                raise FatError(E_NOFS, "no boot signature")
            if b[3:8] == b"EXFAT":
                raise FatError(E_EXFAT, "exFAT volume: reformat the card as FAT32")
            if b[0] in (0xEB, 0xE9) and self._bpb(b, base):
                return
            if pas:
                raise FatError(E_NOFS, "partition has no valid FAT16/FAT32 boot sector")
            other = False
            for i in range(4):
                t, lba = b[446 + 16 * i + 4], struct.unpack_from("<I", b, 446 + 16 * i + 8)[0]
                if t in FAT_TYPES:
                    base = lba
                    break
                other |= t == 0x07
            if not base and other:
                raise FatError(E_EXFAT, "exFAT (or NTFS) partition: reformat the card as FAT32")
            if not base:
                raise FatError(E_NOFS, "no FAT partition in the MBR")

    def _bpb(self, b, base):
        bps, spc, rsvd, nf, rootent, tot16, _media, fsz16 = struct.unpack_from("<HBHBHHBH", b, 11)
        tot = tot16 or struct.unpack_from("<I", b, 32)[0]
        fsz = fsz16 or struct.unpack_from("<I", b, 36)[0]
        if bps != 512 or spc not in (1, 2, 4, 8, 16, 32, 64, 128) or not rsvd or nf not in (1, 2) or not fsz:
            return False
        root_secs = (rootent + 15) >> 4
        meta = rsvd + nf * fsz + root_secs
        if meta >= tot:
            return False
        n = (tot - meta) // spc
        f32 = n >= 65525
        if n < 4085 or (root_secs == 0) != f32:
            return False
        per = 128 if f32 else 256
        if -(-(n + 2) // per) > fsz:
            return False
        self.fs = "fat32" if f32 else "fat16"
        self.base, self.spc, self.clusters, self.root_secs = base, spc, n, root_secs
        self.fat_lba = base + rsvd
        self.data_lba = base + meta
        self.root_lba = self.fat_lba + nf * fsz
        if f32:
            self.root_cluster = struct.unpack_from("<I", b, 44)[0]
            if not 2 <= self.root_cluster < n + 2:
                return False
            ext = struct.unpack_from("<H", b, 40)[0]
            if ext & 0x80 and ext & 15:
                raise FatError(E_NOFS, f"FAT mirroring is off with FAT {ext & 15} live; the game reads "
                                       f"only the first FAT (reformat the card)")
        return True

    def cluster_lba(self, c):
        return self.data_lba + (c - 2) * self.spc

    def next(self, c):
        """Next cluster, 0 at end of chain, 1 for a bad link."""
        f32 = self.fs == "fat32"
        off = c * (4 if f32 else 2)
        b = self.sector(self.fat_lba + off // SECTOR)
        if f32:
            n = struct.unpack_from("<I", b, off % SECTOR)[0] & 0x0FFFFFFF
            if n >= 0x0FFFFFF8:
                return 0
        else:
            n = struct.unpack_from("<H", b, off % SECTOR)[0]
            if n >= 0xFFF8:
                return 0
        return n if 2 <= n < self.clusters + 2 else 1

    def _dir_sectors(self, cluster):
        if not cluster:                                     # FAT16 root region
            yield from range(self.root_lba, self.root_lba + self.root_secs)
            return
        for _ in range(65536 * 32 // SECTOR // self.spc + 1):
            for i in range(self.spc):
                yield self.cluster_lba(cluster) + i
            cluster = self.next(cluster)
            if cluster < 2:
                return

    def listdir(self, cluster=None):
        """[(name, attr, cluster, size)] of the short-name entries in a directory."""
        if cluster is None:
            cluster = self.root_cluster if self.fs == "fat32" else 0
        out = []
        for n, lba in enumerate(self._dir_sectors(cluster)):
            if n >= 4096:
                break
            b = self.sector(lba)
            for i in range(0, SECTOR, 32):
                d = b[i:i + 32]
                if d[0] == 0:
                    return out
                if d[0] == 0xE5 or d[11] & ATTR_LABEL:
                    continue
                hi = struct.unpack_from("<H", d, 20)[0] if self.fs == "fat32" else 0
                lo, size = struct.unpack_from("<HI", d, 26)
                out.append((d[:11], d[11], hi << 16 | lo, size))
        return out

    def find(self, path, is_dir=False):
        """(cluster, size, attr) of an 8.3 path, or FatError(E_NOTFOUND)."""
        cluster = self.root_cluster if self.fs == "fat32" else 0
        parts = [p for p in path.split("/") if p]
        for i, part in enumerate(parts):
            want_dir = i < len(parts) - 1 or is_dir
            sfn = short_name(part)
            hit = next((e for e in self.listdir(cluster) if e[0] == sfn), None)
            if not hit or bool(hit[1] & ATTR_DIR) != want_dir:
                raise FatError(E_NOTFOUND, f"{path}: not found")
            if want_dir:
                cluster = hit[2]
                if not 2 <= cluster < self.clusters + 2:
                    raise FatError(E_CHAIN, f"{path}: bad directory cluster")
        return hit[2], hit[3], hit[1]

    def runs(self, path):
        """Every extent of a file as [(lba, blocks)], trimmed to its size;
        E_CHAIN if the chain is broken, loops or does not end at the size."""
        c, size, _ = self.find(path)
        left = -(-size // SECTOR)
        out = []
        while left:
            if not 2 <= c < self.clusters + 2:
                raise FatError(E_CHAIN, f"{path}: bad cluster {c}")
            lba, k = self.cluster_lba(c), min(left, self.spc)
            if out and out[-1][0] + out[-1][1] == lba:
                out[-1][1] += k
            else:
                out.append([lba, k])
            left -= k
            nx = self.next(c)
            if not left and nx:
                raise FatError(E_CHAIN, f"{path}: chain longer than the file (or looped)")
            c = nx
        return [tuple(r) for r in out]

    def read_file(self, path):
        _, size, _ = self.find(path)
        data = bytearray()
        for lba, n in self.runs(path):
            self.f.seek(lba * SECTOR)
            data += self.f.read(n * SECTOR)
        return bytes(data[:size])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="build an image from files on disk")
    b.add_argument("out")
    b.add_argument("files", nargs="+", metavar="SRC[=NAME]")
    b.add_argument("--fs", default="fat32", choices=("fat16", "fat32"))
    b.add_argument("--superfloppy", action="store_true", help="no MBR: the volume starts at LBA 0")
    b.add_argument("--spc", type=int, help="sectors per cluster (default 4 on FAT16, 1 on FAT32)")
    b.add_argument("--clusters", type=int)
    b.add_argument("--fragment", action="append", default=[], metavar="NAME=PIECES")
    b.add_argument("--seed", type=int, default=1)
    b.add_argument("--lfn", action="store_true", help="add long-name entries")
    b.add_argument("--decoys", action="store_true", help="add deleted/label/fake-LFN decoy entries")
    r = sub.add_parser("runs", help="print the extents of files in an image")
    r.add_argument("image")
    r.add_argument("paths", nargs="+")
    ls = sub.add_parser("ls", help="list a directory of an image")
    ls.add_argument("image")
    ls.add_argument("dir", nargs="?", default="")
    a = ap.parse_args()
    try:
        if a.cmd == "build":
            files = {}
            for spec in a.files:
                src, _, name = spec.partition("=")
                files[name or Path(src).name] = Path(src).read_bytes()
            frag = {k.upper(): int(v) for k, _, v in (s.partition("=") for s in a.fragment)}
            lay = build_image(a.out, files, fs=a.fs, mbr=not a.superfloppy, spc=a.spc,
                              clusters=a.clusters, fragment=frag, seed=a.seed, lfn=a.lfn,
                              decoys=a.decoys)
            print(f"{a.out}: {lay.fs}, {lay.image_sectors * SECTOR:,} B, {lay.clusters} clusters "
                  f"of {lay.spc * SECTOR} B, volume at LBA {lay.part_lba}")
            for name in files:
                if not name.endswith("/"):
                    print(f"  {name}: {len(lay.runs(name))} run(s) {lay.runs(name)[:4]}")
        elif a.cmd == "runs":
            with FatVolume(a.image) as v:
                for p in a.paths:
                    rr = v.runs(p)
                    print(f"{p}: {len(rr)} run(s)", " ".join(f"{lba}+{n}" for lba, n in rr))
        elif a.cmd == "ls":
            with FatVolume(a.image) as v:
                c = v.find(a.dir, is_dir=True)[0] if a.dir else None
                for name, attr, cl, size in v.listdir(c):
                    print(f"{name.decode('latin-1')}  attr={attr:02x} cluster={cl} size={size}")
    except (FatError, ValueError, OSError) as e:
        sys.exit(f"fatimg: {e}")


if __name__ == "__main__":
    main()
