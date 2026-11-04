#!/usr/bin/env python3
"""Make, check and list CHG files, what the SD game menu installs.

A CHG file is a 512-byte header plus the program image exactly as
chgame-upload would write it (spec/chg.md). It is the runtime form: games
are shared as .chgame files, and `chgame cart prepare` writes their CHG
files with the rest of the card (spec/card.md). By hand, copy CHG files into
the card's GAMES folder; the list menu shows the title from the header, the
visual menu the picture packed after the program (--image: a 128x128 PNG
that follows the picture rule, spec/card.md, or a .PIC file). The ones
`chgame cart prepare` writes also carry the game's record (spec/chg.md): what
a backup of the card needs to make the game's cart again.

    python tools/chgpack.py pack build/release/CHFour.ino.bin FOURROW.CHG --title "FOUR IN A ROW" [--image cart.png]
                                                 (or the build's CHFour.ino.elf / .hex; --device rev0 is the default)
    python tools/chgpack.py verify FOURROW.CHG [more.CHG ...]
    python tools/chgpack.py info E:\\              # a mounted card (or any folder)
    python tools/chgpack.py info card.img          # a FAT image: also reports fragmentation

A CHG file names the board its program was built for (its target id:
BOARDS below, spec/chgame.md "Devices and revisions"), and a bootloader
installs only its own board's. Every constant mirrors
platform/bootloader/shared/chg_format.h and src/chgame_map.h; the
bootloader's native tests check that they agree.
"""
from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

MAGIC = 0x31474843          # "CHG1"
FORMAT_VERSION = 1
HEADER_BYTES = 512
LAYOUT_ID = 0x003000F7      # app at 0x3000, metadata page at 0xF700
APP_MAX_SIZE = 50944        # CHGAME_APP_MAX_SIZE
# Board target ids (chg_format.h CHG_TARGET_*): rev0's is named after its MCU
# and keeps that name; later boards are "CGR<n>". Never reused or reassigned.
TARGET_REV0 = 0x35335843    # "CX35": CHGame Rev0 (CH32X035G8U6)
TARGET_REV1 = 0x31524743    # "CGR1": reserved for CHGame Rev1, not yet defined
TARGET_ID = TARGET_REV0     # the board a package is for when none is named
# The boards these tools make and read packages for: target id -> (device,
# layout id, largest payload). A board joins when spec/chgame.md's device
# table defines it, with chcart.model.DEVICES (chcart's tests check they agree).
BOARDS = {TARGET_REV0: ("rev0", LAYOUT_ID, APP_MAX_SIZE)}
# Ids assigned to boards that are not defined yet: refused, but by name.
RESERVED = {TARGET_REV1: "rev1"}
BOOT_SIG = 0x4C424843       # "CHBL" at payload offset 8 marks a bootloader image
TITLE_LEN, AUTHOR_LEN, VERSTR_LEN = 32, 16, 8
IMAGE_OFF = 0x060           # CHG_OFF_IMAGE: offset, bytes, CRC-32 of the game's picture
PICTURE_MAGIC = b"CHB1"     # the picture: MENU.BG's encoding (shared/chgame_card.h)
PICTURE_BYTES = 512 + 128 * 64
PICTURE_MAX_OFFSET = 0x20000   # what the visual menu reads: under 128 KiB, a multiple of 512
RECORD_OFF = 0x06C          # CHG_OFF_RECORD: offset, bytes, CRC-32 of the game's record (JSON; no menu reads it)
RECORD_MAX = 1 << 20

ERRORS = {1: "not a CHG package", 2: "header damaged", 3: "unknown format version",
          4: "built for another board or layout", 5: "bad payload size", 6: "payload damaged",
          7: "a bootloader image, not a program", 8: "picture damaged",
          9: "record damaged"}


class ChgError(Exception):
    def __init__(self, code: int, detail: str = ""):
        self.code = code
        super().__init__(ERRORS[code] + (f": {detail}" if detail else ""))


def _text(s: str, n: int, what: str) -> bytes:
    b = s.encode("ascii")
    if len(b) >= n or any(c < 32 or c > 126 for c in b):
        raise ValueError(f"{what} must be printable ASCII, at most {n - 1} characters")
    return b.ljust(n, b"\0")


def fourcc(v: int) -> str:
    """A 32-bit id as the four characters it spells (little-endian), or '?'."""
    b = v.to_bytes(4, "little")
    return b.decode("ascii") if all(32 < c < 127 for c in b) else "?"


def target_of(device: str) -> int:
    """The target id of a device the tools build for ("rev0"), else ValueError."""
    for t, (d, _, _) in BOARDS.items():
        if d == device:
            return t
    if device in RESERVED.values():
        raise ValueError(f"{device} is reserved for a board that is not defined yet (spec/chgame.md)")
    raise ValueError(f"unknown device {device!r} (known: {', '.join(d for d, _, _ in BOARDS.values())})")


def describe_target(target: int) -> str:
    """'rev0 (CX35)', or what an id this module cannot use is."""
    if target in BOARDS:
        return f"{BOARDS[target][0]} ({fourcc(target)})"
    if target in RESERVED:
        return f"{RESERVED[target]} ({fourcc(target)}), a board these tools do not support yet"
    return f"an unknown board, target 0x{target:08X} ({fourcc(target)})"


def pad_image(image: bytes) -> bytes:
    """The chgame-upload rule: pad with 0xFF to a multiple of 4."""
    return image + b"\xff" * (-len(image) % 4)


def pack(image: bytes, title: str, author: str = "", version: str = "", app_version: int = 0,
         picture: bytes | None = None, record: bytes | None = None, target: int = TARGET_ID) -> bytes:
    """The CHG file: header, payload and, given one (a picture in MENU.BG's
    encoding, as chcart.runtime.menu_picture() makes from a PNG), the game's
    picture at the next 512-byte boundary after the payload; then, given one
    (chcart.backup.record() makes it), the game's record at the next 512-byte
    boundary after that (spec/chg.md). `target` is the board the image was
    built for (BOARDS; target_of("rev0") gives one)."""
    if target not in BOARDS:
        raise ValueError(f"cannot pack for {describe_target(target)}")
    _, layout, max_size = BOARDS[target]
    payload = pad_image(image)
    if not payload or len(payload) > max_size:
        raise ValueError(f"image is {len(payload)} B; the limit is {max_size} B")
    if len(payload) >= 12 and struct.unpack_from("<I", payload, 8)[0] == BOOT_SIG:
        raise ValueError("this is a bootloader image, not a program")
    h = bytearray(HEADER_BYTES)
    struct.pack_into("<IHHIIIIII", h, 0, MAGIC, FORMAT_VERSION, HEADER_BYTES, target, layout,
                     len(payload), zlib.crc32(payload) & 0xFFFFFFFF, app_version & 0xFFFFFFFF, 0)
    h[0x20:0x40] = _text(title, TITLE_LEN, "title")
    h[0x40:0x50] = _text(author, AUTHOR_LEN, "author")
    h[0x50:0x58] = _text(version, VERSTR_LEN, "version")
    body = bytearray(payload)

    def section(off, data):
        at = -(-(HEADER_BYTES + len(body)) // 512) * 512
        struct.pack_into("<III", h, off, at, len(data), zlib.crc32(data) & 0xFFFFFFFF)
        body.extend(bytes(at - HEADER_BYTES - len(body)) + data)

    if picture is not None:
        if len(picture) != PICTURE_BYTES or picture[:4] != PICTURE_MAGIC:
            raise ValueError(f"the picture must be {PICTURE_BYTES} B starting with CHB1 (MENU.BG's encoding)")
        section(IMAGE_OFF, picture)
    if record is not None:
        if not record or len(record) > RECORD_MAX:
            raise ValueError(f"the record is {len(record)} B; 1 to {RECORD_MAX} B")
        section(RECORD_OFF, record)
    struct.pack_into("<I", h, 0x1FC, zlib.crc32(bytes(h[:0x1FC])) & 0xFFFFFFFF)
    return bytes(h) + bytes(body)


def read_picture(data: bytes) -> bytes | None:
    """The game's picture in a CHG file (checked with parse() first), or None
    if it has none. ChgError(8) if the field names one that is not there as
    spec/chg.md says: the visual menu then shows its no-picture screen, and
    the game still installs."""
    at, n, crc = struct.unpack_from("<III", data, IMAGE_OFF)
    if not (at or n or crc):
        return None
    payload_end = HEADER_BYTES + struct.unpack_from("<I", data, 0x10)[0]
    pic = data[at:at + n]
    if (n != PICTURE_BYTES or at % 512 or at < payload_end or at >= PICTURE_MAX_OFFSET or len(pic) != n
            or pic[:4] != PICTURE_MAGIC or zlib.crc32(pic) & 0xFFFFFFFF != crc):
        raise ChgError(8, f"{n} B at {at}")
    return pic


def read_record(data: bytes) -> bytes | None:
    """The game's record in a CHG file (checked with parse() first): the
    JSON's bytes, or None if it has none. ChgError(9) if the field names one
    that is not there as spec/chg.md says: a backup then treats the file as
    one without a record."""
    at, n, crc = struct.unpack_from("<III", data, RECORD_OFF)
    if not (at or n or crc):
        return None
    end = HEADER_BYTES + struct.unpack_from("<I", data, 0x10)[0]
    pic_at, pic_n, _ = struct.unpack_from("<III", data, IMAGE_OFF)
    if pic_at or pic_n:
        end = max(end, pic_at + pic_n)
    rec = data[at:at + n]
    if (not n or n > RECORD_MAX or at % 512 or at < end or len(rec) != n
            or zlib.crc32(rec) & 0xFFFFFFFF != crc):
        raise ChgError(9, f"{n} B at {at}")
    return rec


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def parse(data: bytes, check_payload: bool = True, target: int | None = None) -> dict:
    """Checks a package the way the bootloader does (same order, same codes).
    A bootloader takes only its own board's packages: pass `target` to check
    as that board's would. None takes any board in BOARDS, and the result's
    "device" says which."""
    if len(data) < HEADER_BYTES or struct.unpack_from("<I", data, 0)[0] != MAGIC:
        raise ChgError(1)
    h = data[:HEADER_BYTES]
    if zlib.crc32(h[:0x1FC]) & 0xFFFFFFFF != struct.unpack_from("<I", h, 0x1FC)[0]:
        raise ChgError(2)
    magic, ver, hb, found, layout, n, crc, appver, flags = struct.unpack_from("<IHHIIIIII", h, 0)
    if ver != FORMAT_VERSION or hb != HEADER_BYTES:
        raise ChgError(3, f"version {ver}, header {hb} B")
    if found not in BOARDS or (target is not None and found != target):
        raise ChgError(4, f"built for {describe_target(found)}"
                       + (f", not {describe_target(target)}" if target is not None and found in BOARDS else ""))
    device, board_layout, max_size = BOARDS[found]
    if layout != board_layout:
        raise ChgError(4, f"layout 0x{layout:08X}; {device}'s is 0x{board_layout:08X}")
    if not n or n > max_size or n & 3 or len(data) < HEADER_BYTES + n:
        raise ChgError(5, f"{n} B in a {len(data)} B file")
    info = {"device": device, "target": found,
            "title": _cstr(h[0x20:0x40]), "author": _cstr(h[0x40:0x50]), "version": _cstr(h[0x50:0x58]),
            "payload_bytes": n, "payload_crc32": crc, "app_version": appver, "file_bytes": len(data)}
    if check_payload:
        payload = data[HEADER_BYTES:HEADER_BYTES + n]
        if zlib.crc32(payload) & 0xFFFFFFFF != crc:
            raise ChgError(6)
        if len(payload) >= 12 and struct.unpack_from("<I", payload, 8)[0] == BOOT_SIG:
            raise ChgError(7)
    return info


def _describe(name: str, data: bytes) -> tuple[bool, str]:
    try:
        i = parse(data)
        pic = read_picture(data)
        rec = read_record(data)
    except ChgError as e:
        return False, f"{name:14s} BAD  {e}"
    extra = " ".join(x for x in (i["author"], i["version"]) if x)
    board = f"  [{i['device']}]" if i["target"] != TARGET_REV0 else ""
    return True, (f"{name:14s} ok   {i['payload_bytes']:6d} B  {i['title']}" + (f"  ({extra})" if extra else "") + board
                  + ("  [picture]" if pic else "") + ("  [record]" if rec else ""))


def picture_file(path: str) -> bytes:
    """--image: a PNG made into a picture (chcart, the picture rule), or a
    finished one (a .PIC file, MENU.BG's encoding)."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] == PICTURE_MAGIC:
        return data
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from chcart import runtime           # (lazily: chcart imports this module)
    from chcart.model import CartError
    try:
        return runtime.menu_picture(data)
    except CartError as e:
        raise ValueError(f"{path}: {e}") from None


def program_image(path: str) -> bytes:
    """The program image in a .bin, or in the .hex or .elf the build writes
    beside it (chcart converts those: the segments at the load address)."""
    if pathlib.Path(path).suffix.lower() in (".hex", ".elf"):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        from chcart import sources           # (lazily: chcart imports this module)
        return sources.image_from_file(path)
    return pathlib.Path(path).read_bytes()


def cmd_pack(a) -> int:
    image = program_image(a.bin)
    out = pack(image, a.title, a.author, a.version, a.app_version, picture_file(a.image) if a.image else None,
               target=target_of(a.device))
    pathlib.Path(a.out).write_bytes(out)
    info = parse(out)
    print(f"{a.out}: {a.title!r}, {info['payload_bytes']} B payload, crc 0x{info['payload_crc32']:08X}"
          + (", with its picture" if a.image else ""))
    return 0


def cmd_verify(a) -> int:
    bad = 0
    for f in a.files:
        ok, line = _describe(pathlib.Path(f).name, pathlib.Path(f).read_bytes())
        print(line)
        bad += not ok
    return 1 if bad else 0


def cmd_info(a) -> int:
    p = pathlib.Path(a.where)
    bad = 0
    if p.is_dir():
        games = p / "GAMES" if (p / "GAMES").is_dir() else p
        files = sorted(f for f in games.iterdir() if f.suffix.upper() == ".CHG")
        print(f"{games}: {len(files)} package(s)")
        for f in files:
            ok, line = _describe(f.name, f.read_bytes())
            print("  " + line)
            bad += not ok
        return 1 if bad else 0
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "platform" / "board" / "arduino" / "CHGame" / "libraries" / "CHSd" / "tools"))
    import fatimg  # noqa: E402
    vol = fatimg.FatVolume(str(p))
    games, _, _ = vol.find("GAMES", is_dir=True)
    names = []
    for raw, attr, _, _ in vol.listdir(games):
        if attr & (fatimg.ATTR_DIR | fatimg.ATTR_HIDDEN | fatimg.ATTR_SYS) or raw[8:11] != b"CHG":
            continue
        base = raw[:8].decode("ascii", "replace").rstrip()
        names.append(base + ".CHG")
    print(f"{p}: {len(names)} package(s) in GAMES")
    for n in sorted(names):
        path = "GAMES/" + n
        runs = vol.runs(path)
        ok, line = _describe(n, vol.read_file(path))
        frag = f"  [{len(runs)} fragment{'s' if len(runs) != 1 else ''}]"
        print("  " + line + frag)
        bad += not ok
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack", help="wrap a program image in a package")
    p.add_argument("bin", help="the program: a .bin, or the build's .hex or .elf")
    p.add_argument("out")
    p.add_argument("--title", required=True, help="shown in the menu (31 characters at most; 20 fit on screen)")
    p.add_argument("--author", default="")
    p.add_argument("--version", default="")
    p.add_argument("--app-version", type=lambda s: int(s, 0), default=0)
    p.add_argument("--image", help="the game's picture for the visual menu: a 128x128 PNG or a .PIC file")
    p.add_argument("--device", default="rev0", help="the board the program was built for (default rev0)")
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser("verify", help="check packages")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_verify)
    p = sub.add_parser("info", help="list the packages on a card, a folder or a FAT image")
    p.add_argument("where")
    p.set_defaults(fn=cmd_info)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
