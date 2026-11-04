"""Research tool for chgscan.h: list every lui/addi four-character constant in .CHG payloads with the
instruction context around it, to see which features tell a save code from other ASCII constants.

    python tools/scan_eval.py <file.CHG> ...        # every candidate with its features
"""
import sys
from pathlib import Path


def le16(b, i):
    return b[i] | b[i + 1] << 8


def le32(b, i):
    return le16(b, i) | le16(b, i + 2) << 16


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def ok(c):
    return all(48 <= x <= 57 or 65 <= x <= 90 for x in c)


def decode(b, i):
    """(length, kind, info) of the instruction at i. kind in: bra(rs1,rs2) sw(rs2,off) call ret mv(rd,rs) li(rd,imm)
    lw(rd,rs1,imm) addi(rd,rs1,imm) wr(rd) other."""
    h = le16(b, i)
    if h & 3 == 3:
        w = le32(b, i)
        op, rd, f3, rs1, rs2 = w & 0x7F, (w >> 7) & 31, (w >> 12) & 7, (w >> 15) & 31, (w >> 20) & 31
        if op == 0x63:
            return 4, "bra", (rs1, rs2, f3)
        if op == 0x23:
            return 4, "sw" if f3 == 2 else "st", (rs2, rs1, sx(((w >> 25) << 5) | rd, 12))
        if op == 0x6F:
            return 4, "call" if rd == 1 else "jmp", rd
        if op == 0x67:
            return 4, "call" if rd == 1 else "jmp", rd
        if op == 0x13 and f3 == 0:
            return 4, "addi", (rd, rs1, sx(w >> 20, 12))
        if op == 0x03:
            return 4, "lw", (rd, rs1, sx(w >> 20, 12))
        if op == 0x33 and (w >> 25) == 0 and f3 == 0:
            return 4, "add", (rd, rs1, rs2)
        return 4, "wr" if op in (0x37, 0x17, 0x13, 0x33, 0x03) else "other", rd
    q, f3 = h & 3, h >> 13
    r = (h >> 7) & 31
    r2 = (h >> 2) & 31
    if q == 1 and f3 == 1:
        return 2, "call", 1                      # c.jal
    if q == 1 and f3 == 5:
        return 2, "jmp", 0                       # c.j
    if q == 1 and f3 in (6, 7):
        return 2, "bra", (8 + ((h >> 7) & 7), 0, f3)
    if q == 1 and f3 == 2:
        return 2, "li", (r, sx(((h >> 12) & 1) << 5 | r2, 6))
    if q == 2 and f3 == 4:
        if (h >> 12) & 1 == 0:
            return (2, "mv", (r, r2)) if r2 else (2, "jmp", 0)
        if r2 == 0 and r:
            return 2, "call", 1                  # c.jalr
        if r2:
            return 2, "wr", r                    # c.add
        return 2, "other", 0
    if q == 0 and f3 == 6:
        return 2, "sw", (8 + ((h >> 2) & 7), 8 + ((h >> 7) & 7), 0)
    if q == 2 and f3 == 6:
        return 2, "sw", (r2, 2, 0)
    if q == 0 and f3 == 2:
        return 2, "wr", 8 + ((h >> 2) & 7)
    if q == 0 and f3 == 0:
        return 2, "wr", 8 + ((h >> 2) & 7)
    if q == 1 and f3 in (0, 3):
        return 2, "wr", r
    if q == 1 and f3 == 4:
        return 2, "wr", 8 + ((h >> 7) & 7)
    if q == 2 and f3 in (0, 2):
        return 2, "wr", r
    return 2, "other", 0


def candidates(p):
    out = []
    for i in range(0, len(p) - 8, 2):
        w = le32(p, i)
        if w & 0x7F != 0x37:
            continue
        rd = (w >> 7) & 31
        if not rd:
            continue
        for q in range(i + 4, min(i + 28, len(p) - 3), 2):
            a = le32(p, q)
            if a & 0x707F == 0x13 and (a >> 7) & 31 == rd and (a >> 15) & 31 == rd:
                v = ((w & 0xFFFFF000) + sx(a >> 20, 12)) & 0xFFFFFFFF
                c = bytes((v >> (8 * k)) & 255 for k in range(4))
                if ok(c):
                    out.append((i, q, rd, c.decode()))
                break
    return out


def features(p, i, q, rd):
    """Uses of rd after the addi (until rd is overwritten), up to 12 instructions."""
    f = []
    j = q + 4
    regs = {rd}
    loaded = {}                                  # reg -> offset of an lw seen just before / after the pair
    for b, want in ((i - 2, 2), (i - 4, 4)):
        if b >= 0:
            n, k, inf = decode(p, b)
            if n == want and k == "lw":
                loaded[inf[0]] = inf[2]
            elif n == want and want == 2 and (le16(p, b) & 0xE003) == 0x4000:   # c.lw
                h = le16(p, b)
                off = ((h >> 10) & 7) << 3 | ((h >> 6) & 1) << 2 | ((h >> 5) & 1) << 6
                loaded[8 + ((h >> 2) & 7)] = off
    for _ in range(14):
        if j + 2 > len(p):
            break
        n, k, inf = decode(p, j)
        h = le16(p, j)
        if n == 2 and (h & 0xE003) == 0x4000:
            off = ((h >> 10) & 7) << 3 | ((h >> 6) & 1) << 2 | ((h >> 5) & 1) << 6
            loaded[8 + ((h >> 2) & 7)] = off
        if k == "lw":
            loaded[inf[0]] = inf[2]
        if k == "bra" and (inf[0] in regs or inf[1] in regs):
            o = inf[1] if inf[0] in regs else inf[0]
            f.append("CMP" + ("lw%x" % loaded[o] if o in loaded else ""))
        elif k == "sw" and inf[0] in regs:
            f.append("SW%d" % inf[2])
        elif k == "mv" and inf[1] in regs:
            regs.add(inf[0])
            if inf[0] == 10:
                f.append("MVA0")
            elif inf[0] in (8, 9) or 18 <= inf[0] <= 27:
                f.append("MVS")
        elif k in ("call", "jmp") and 10 in regs:
            li1 = False
            m = i + 4
            while m < j:
                n2, k2, inf2 = decode(p, m)
                if (k2 == "li" and inf2[0] == 11) or (k2 == "addi" and inf2[0] == 11 and inf2[1] == 0):
                    li1 = True
                m += n2
            f.append("A0" + k.upper() + ("li1" if li1 else ""))
            break
        elif k == "call":
            f.append("CALL")
            break
        elif k == "lw" and inf[2] in (0x500, 0x600, 0x504, 0x604):
            f.append("PAGE")
        elif k == "jmp":
            f.append("J")
            break
        if k in ("wr", "addi", "lw", "li", "mv", "add") and not (k == "mv" and inf[1] in regs):
            d = inf if isinstance(inf, int) else inf[0]
            if d in regs:
                regs.discard(d)
                if not regs:
                    break
        j += n
    # before: a page load in the 40 bytes before the lui (aligned decode is unsure: scan halfwords)
    for b in range(max(0, i - 40), i, 2):
        n, k, inf = decode(p, b)
        if k in ("lw", "addi") and inf[2] in (0x500, 0x600):
            f.insert(0, "pPAGE")
            break
    return f


def main():
    for a in sys.argv[1:]:
        d = Path(a).read_bytes()
        n = le32(d, 0x10)
        t = d[0x20:0x40].split(b"\0")[0].decode("ascii", "replace")
        p = d[512:512 + n]
        print("== %s \"%s\"" % (a, t))
        for i, q, rd, c in candidates(p):
            print("   %06x %s x%-2d %s" % (i, c, rd, " ".join(features(p, i, q, rd))))


if __name__ == "__main__":
    main()
