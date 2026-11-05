"""Writes out/pubcard: a simulator card with public games only (CHGame's examples, CHix, StarLift, CHarth), for
the README pictures. tools/pictures.py runs this; "Demo Game" has a code outside Names.h, so the scan finds a new name."""
import shutil
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "out" / "pubcard"


def call(code):
    """lui a0, hi; c.li a1, 1; addi a0, a0, lo; jal ra: the magic and version handed to save::read/write."""
    v = struct.unpack("<I", code.encode())[0]
    hi = (v + 0x800) & 0xFFFFF000
    lo = (v - hi) & 0xFFF
    w = [hi | 10 << 7 | 0x37, lo << 20 | 10 << 15 | 10 << 7 | 0x13, 0x100 << 20 | 1 << 7 | 0x6F]
    return struct.pack("<I", w[0]) + struct.pack("<H", 0x4585) + struct.pack("<I", w[1]) + struct.pack("<I", w[2])


def chg(path, title, code):
    payload = (call(code) if code else b"") + bytes(4000)
    head = bytearray(512)
    head[0:4] = b"CHG1"
    head[0x10:0x14] = struct.pack("<I", len(payload))
    head[0x20:0x20 + len(title)] = title.encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(head) + payload)


GAMES = [("CHIX.CHG", "CHix", "CHIX"), ("STARLIFT.CHG", "StarLift", "SLFT"), ("CHARTH.CHG", "CHarth", "CHAR"),
         ("CASINO/BLACKJ.CHG", "Blackjack", "CHBJ"), ("CASINO/POKER.CHG", "Poker", "CHPK"),
         ("CASINO/ROULETTE.CHG", "Roulette", "CHRL"), ("CASINO/SLOTS.CHG", "Slots", "CHSL"),
         ("BOARD/CHESS.CHG", "Chess", "CHCS"), ("BOARD/CHECKERS.CHG", "Checkers", "CHCK"),
         ("BOARD/MAHJONG.CHG", "Mahjong", "CHMJ"), ("WORDS.CHG", "Words", "CHWD"), ("YACHT.CHG", "Yacht Dice", "CHYD"),
         ("DEMO.CHG", "Demo Game", "DEMO"), ("HELLO.CHG", "Hello", None)]

shutil.rmtree(OUT, ignore_errors=True)
for p, t, c in GAMES:
    chg(OUT / "GAMES" / p, t, c)
print(OUT, len(GAMES), "games")
