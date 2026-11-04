"""Rewrite the sim card's fake game savm_card/GAMES/ZED.CHG ("Zed Game", save code ZZZZ) so that its payload
looks like a real save call, which is what chgscan.h now requires (a bare lui/addi pair is no longer a code):
    lui a0,%hi("ZZZZ"); c.li a1,1; addi a0,a0,%lo("ZZZZ"); jal ra,+0x100
    python tools/tests/make_zed.py          (from the CHSaveMan/CHSaveMan folder)
"""
import struct
from pathlib import Path

ZED = Path(__file__).resolve().parents[3] / "savm_card" / "GAMES" / "ZED.CHG"
v = struct.unpack("<I", b"ZZZZ")[0]
hi = (v + 0x800) & 0xFFFFF000
lo = (v - hi) & 0xFFF
code = struct.pack("<I", hi | 10 << 7 | 0x37) + struct.pack("<H", 0x4585) + \
    struct.pack("<I", lo << 20 | 10 << 15 | 10 << 7 | 0x13) + struct.pack("<I", 0x100 << 20 | 1 << 7 | 0x6F)
payload = code + bytes(72 - len(code))
head = bytearray(ZED.read_bytes()[:512])
head[0x10:0x14] = struct.pack("<I", len(payload))
ZED.write_bytes(bytes(head) + payload)
print("wrote", ZED, len(payload), "payload bytes")
