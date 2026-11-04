"""CHG packages for the SD game menu: `chgame-upload pack`.

A package is a 512-byte header plus the program image as `flash` would write
it (spec/chg.md in the CHGame repository). The
board package runs this after every build, so *Sketch > Export Compiled
Binary* leaves a `.chg` beside the `.bin`: copy it into the card's GAMES
folder and the menu lists it.

The constants mirror platform/bootloader/shared/chg_format.h, as
tools/chgpack.py does; test/protocol checks the three agree.
"""
from __future__ import annotations

import os
import struct
import zlib

from . import layout as L

MAGIC = 0x31474843          # "CHG1"
FORMAT_VERSION = 1
HEADER_BYTES = 512
LAYOUT_ID = 0x003000F7      # app at 0x3000, metadata page at 0xF700
BOOT_SIG = 0x4C424843       # "CHBL" at payload offset 8 marks a bootloader image
TITLE_LEN, AUTHOR_LEN, VERSTR_LEN = 32, 16, 8

# Board target ids (chg_format.h CHG_TARGET_*): which board a program, a
# package or a bootloader is for. Rev0's is named after its MCU and keeps
# that name; later boards are "CGR<n>" (spec/chgame.md, "Devices and
# revisions"). Never reused or reassigned.
TARGET_REV0 = 0x35335843    # "CX35": CHGame Rev0 (CH32X035G8U6)
TARGET_REV1 = 0x31524743    # "CGR1": reserved for CHGame Rev1, not yet defined
TARGET_ID = TARGET_REV0     # what pack writes when no device is named
DEVICES = {"rev0": TARGET_REV0}          # the boards this tool uploads and packs for
RESERVED = {"rev1": TARGET_REV1}         # assigned, not defined yet: refused by name


def fourcc(v: int) -> str:
    """A 32-bit id as the four characters it spells (little-endian), or '?'."""
    b = v.to_bytes(4, "little")
    return b.decode("ascii") if all(32 < c < 127 for c in b) else "?"


def board_name(target: int) -> str:
    """'rev0 (CX35)'; 'rev1 (CGR1)' for a reserved one; the id otherwise."""
    for name, t in list(DEVICES.items()) + list(RESERVED.items()):
        if t == target:
            return f"{name} ({fourcc(t)})"
    return f"unknown board 0x{target:08X} ({fourcc(target)})"


def target_of(device: str) -> int:
    """The target id of a device this tool knows ("rev0"), else ValueError."""
    if device in DEVICES:
        return DEVICES[device]
    if device in RESERVED:
        raise ValueError(f"{device} is reserved for a board that is not defined yet")
    raise ValueError(f"unknown device {device!r} (known: {', '.join(DEVICES)})")


def _text(s: str, n: int, what: str) -> bytes:
    b = s.encode("ascii", "replace")
    if len(b) >= n or any(c < 32 or c > 126 for c in b):
        raise ValueError(f"{what} must be printable ASCII, at most {n - 1} characters")
    return b.ljust(n, b"\0")


def default_title(path: str) -> str:
    """`MyGame.ino.bin` -> `MYGAME`: the sketch's name, as the menu's capitals."""
    base = os.path.basename(path).split(".", 1)[0]
    return base.upper()[:TITLE_LEN - 1] or "PROGRAM"


def default_output(path: str) -> str:
    """`MyGame.ino.bin` -> `MyGame.ino.chg`, beside it (Export Compiled Binary
    copies every `<project>.*` file from the build folder)."""
    root, ext = os.path.splitext(path)
    return (root if ext.lower() == ".bin" else path) + ".chg"


def pack(image: bytes, title: str, author: str = "", version: str = "", app_version: int = 0,
         device: str = "rev0") -> bytes:
    target = target_of(device)
    payload = image + bytes([L.ERASED]) * (-len(image) % 4)
    if not payload or len(payload) > L.APP_MAX_SIZE:
        raise ValueError(f"image is {len(payload)} B; the limit is {L.APP_MAX_SIZE} B")
    if len(payload) >= 12 and struct.unpack_from("<I", payload, 8)[0] == BOOT_SIG:
        raise ValueError("this is a bootloader image, not a program")
    h = bytearray(HEADER_BYTES)
    struct.pack_into("<IHHIIIIII", h, 0, MAGIC, FORMAT_VERSION, HEADER_BYTES, target, LAYOUT_ID,
                     len(payload), zlib.crc32(payload) & 0xFFFFFFFF, app_version & 0xFFFFFFFF, 0)
    h[0x20:0x40] = _text(title, TITLE_LEN, "title")
    h[0x40:0x50] = _text(author, AUTHOR_LEN, "author")
    h[0x50:0x58] = _text(version, VERSTR_LEN, "version")
    struct.pack_into("<I", h, 0x1FC, zlib.crc32(bytes(h[:0x1FC])) & 0xFFFFFFFF)
    return bytes(h) + payload
