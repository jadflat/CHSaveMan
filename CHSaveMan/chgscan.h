// Finds a game's flash save code(s) in a .CHG payload. Pure: no I/O, no heap, no float; board and host.
//
// A .CHG = 512-byte header ("CHG1" at 0, payload length u32 LE at 0x10, title 32 B ASCII NUL-padded at
// 0x20) + the RISC-V payload. The save code (save::magic("XXXX")) is a 32-bit constant built by
//   lui  rd, imm20        (opcode 0x37)
//   addi rd2, rd, imm12   (opcode 0x13, funct3 0; rd2 usually rd), within the next PAIR bytes
// Many four-character constants are built that way; what marks the save code is how it is USED in the
// few instructions after the pair (tools/scan_eval.py prints these for any .CHG):
//   SURE  8  compared (beq/bne) with a word just loaded from offset 0x500/0x600 of a register: the
//            library's record check at the save pages 0xF500/0xF600, inlined with the constant
//            (every saving game built with the library up to 0.3 has two of these).
//   CALL  4  left in a0 for a call (or tail jump) with a small constant just put in a1: the magic and
//            version handed to save::read/write (the library with save backends, after 0.3).
//   WORD  3  compared with a word just loaded from offset 0 of a pointer: a record check that takes
//            the page as an argument (older or hand-written save code).
// Anything else (stored, compared with other values, passed elsewhere) scores nothing and is not kept.
// The codes come out best first (score, then how often), and when one reaches SURE only the SURE ones
// are kept: nCodes is then almost always 1. nCodes 0: the game does not save to flash (or the scan
// could not tell). Codes are four printable characters (no space, no '=').
//
// Use:  ChgScan s; s.begin();  loop { read up to CHUNK bytes into s.chunk(); s.feed(n); }  s.finish();
// The last AHEAD unscanned bytes (and BACK before them) are carried to the front of buf for the next chunk.
#pragma once
#include <stdint.h>
#include <string.h>

namespace chgscan {

static const uint16_t CHUNK = 512;
static const uint8_t BACK = 12;                 // bytes before a lui we look at (the record load, up to ~3 instructions)
static const uint8_t AHEAD = 64;                // bytes from a lui where the pair and its uses must lie
static const uint8_t PAIR = 24;                 // bytes after the lui where the addi may sit
static const uint8_t CARRY = BACK + AHEAD + 4;
static const uint8_t MAXCODES = 6;
enum : uint8_t { WORD = 3, CALL = 4, SURE = 8 };
static const uint8_t NEXCLUDE = 5;              // the library's own constants
static const char EXCLUDE[NEXCLUDE][4] = { { 'B', 'G', 'H', 'C' }, { 'E', 'X', 'F', 'A' }, { '3', '3', '3', '3' },
                                           { 'U', 'U', 'U', 'U' }, { 'C', 'H', 'G', 'F' } };

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// Checks a 512-byte header. Fills title (trimmed, NUL-terminated, non-printables -> '?') and the
/// payload length; false if the tag is not "CHG1".
static inline bool header(const uint8_t *h, char title[33], uint32_t *payload) {
    if (h[0] != 'C' || h[1] != 'H' || h[2] != 'G' || h[3] != '1') return false;
    *payload = le32(h + 0x10);
    const uint8_t *t = h + 0x20;
    uint8_t a = 0, b = 32;
    while (b > 0 && (t[b - 1] == 0 || t[b - 1] == ' ')) b--;
    while (a < b && t[a] == ' ') a++;
    uint8_t n = 0;
    for (uint8_t i = a; i < b; i++) title[n++] = (t[i] >= 32 && t[i] < 127) ? (char)t[i] : '?';
    title[n] = 0;
    return true;
}

// Just enough RV32EC decoding: the register an instruction writes, and the few kinds the scan follows.
enum Kind : uint8_t { K_OTHER, K_LW, K_LI, K_ADDI, K_MV, K_BRANCH, K_CALL, K_STOP };
struct Ins {
    uint8_t len, kind, rd, rs1, rs2;   // rd 0: writes nothing we follow
    int16_t imm;                       // K_LW: offset; K_ADDI/K_LI (32-bit): immediate
};

static inline void decode(const uint8_t *b, Ins &i) {
    uint16_t h = (uint16_t)(b[0] | b[1] << 8);
    i.kind = K_OTHER; i.rd = i.rs1 = i.rs2 = 0; i.imm = 0;
    if ((h & 3) == 3) {
        uint32_t w = le32(b);
        uint8_t op = w & 0x7F, f3 = (w >> 12) & 7;
        i.len = 4;
        i.rd = (w >> 7) & 31; i.rs1 = (w >> 15) & 31; i.rs2 = (w >> 20) & 31;
        i.imm = (int16_t)((int32_t)w >> 20);
        switch (op) {
            case 0x63: i.kind = K_BRANCH; i.rd = 0; break;
            case 0x6F: case 0x67: i.kind = i.rd == 1 ? K_CALL : K_STOP; i.rd = 0; break;  // jal t0: millicode
            case 0x03: if (f3 == 2) i.kind = K_LW; break;
            case 0x13: if (f3 == 0) i.kind = i.rs1 == 0 ? K_LI : K_ADDI; break;
            case 0x37: case 0x17: case 0x33: break;
            default: i.rd = 0;
        }
        return;
    }
    uint8_t q = h & 3, f3 = h >> 13, r = (h >> 7) & 31, r2 = (h >> 2) & 31;
    i.len = 2;
    if (q == 0) {
        if (f3 == 0 || f3 == 2) i.rd = 8 + ((h >> 2) & 7);                       // c.addi4spn, c.lw
        if (f3 == 2) { i.kind = K_LW; i.imm = (int16_t)(((h >> 10) & 7) << 3 | ((h >> 6) & 1) << 2 | ((h >> 5) & 1) << 6); }
    } else if (q == 1) {
        if (f3 == 1) i.kind = K_CALL;                                             // c.jal
        else if (f3 == 5) i.kind = K_STOP;                                        // c.j
        else if (f3 == 2) { i.kind = K_LI; i.rd = r; }                            // c.li
        else if (f3 == 0 || f3 == 3) i.rd = r;                                    // c.addi, c.lui
        else if (f3 == 4) i.rd = 8 + ((h >> 7) & 7);                              // c.srli .. c.and
    } else if (q == 2) {
        if (f3 == 4) {
            if (!((h >> 12) & 1)) {
                if (r2) { i.kind = K_MV; i.rd = r; i.rs1 = r2; } else i.kind = K_STOP;   // c.mv, c.jr
            } else if (!r2) i.kind = r ? K_CALL : K_STOP;                         // c.jalr, c.ebreak
            else i.rd = r;                                                        // c.add
        } else if (f3 == 0 || f3 == 2) i.rd = r;                                  // c.slli, c.lwsp
    }
}

struct ChgScan {
    uint8_t buf[CARRY + CHUNK];
    uint32_t pos;                 // payload offset of buf[0]
    uint16_t carry;               // bytes kept at the front of buf
    uint8_t back;                 // of which already scanned (context before the next lui)
    uint8_t nCodes;
    char codes[MAXCODES][4];      // best first after finish()
    uint8_t score[MAXCODES];      // best evidence of each: SURE, CALL or WORD
    uint8_t hits[MAXCODES];       // how many times it was seen with that evidence or better

    void begin() { pos = 0; carry = 0; back = 0; nCodes = 0; }
    /// Where the next chunk goes (room for CHUNK bytes).
    uint8_t *chunk() { return buf + carry; }
    /// n (<= CHUNK) bytes were written at chunk().
    void feed(uint16_t n) { run(carry + n, false); }
    /// End of payload: scan what is left, then rank (best first; only SURE codes if there is one).
    void finish() {
        run(carry, true);
        carry = 0;
        for (uint8_t i = 1; i < nCodes; i++)          // insertion sort by (score, hits), stable
            for (uint8_t j = i; j > 0 && (score[j] > score[j - 1] || (score[j] == score[j - 1] && hits[j] > hits[j - 1])); j--) {
                char c[4]; memcpy(c, codes[j], 4); memcpy(codes[j], codes[j - 1], 4); memcpy(codes[j - 1], c, 4);
                uint8_t t = score[j]; score[j] = score[j - 1]; score[j - 1] = t;
                t = hits[j]; hits[j] = hits[j - 1]; hits[j - 1] = t;
            }
        if (nCodes && score[0] >= SURE)
            while (score[nCodes - 1] < SURE) nCodes--;
    }

  private:
    void add(uint32_t v, uint8_t s) {
        char c[4];
        for (uint8_t i = 0; i < 4; i++) c[i] = (char)(v >> (8 * i));
        for (uint8_t i = 0; i < NEXCLUDE; i++)
            if (!memcmp(c, EXCLUDE[i], 4)) return;
        for (uint8_t i = 0; i < nCodes; i++)
            if (!memcmp(c, codes[i], 4)) {
                if (s > score[i]) { score[i] = s; hits[i] = 1; }
                else if (s == score[i] && hits[i] < 255) hits[i]++;
                return;
            }
        uint8_t k = nCodes;
        if (k == MAXCODES) {                          // full: replace the weakest if this beats it
            k = 0;
            for (uint8_t i = 1; i < MAXCODES; i++) if (score[i] < score[k]) k = i;
            if (score[k] >= s) return;
        } else nCodes++;
        memcpy(codes[k], c, 4); score[k] = s; hits[k] = 1;
    }

    static bool printable(uint32_t v) {
        for (uint8_t i = 0; i < 4; i++) {
            uint8_t c = (uint8_t)(v >> (8 * i));
            if (c <= ' ' || c >= 127 || c == '=') return false;
        }
        return true;
    }

    // The lui at buf[p] (bytes up to end are here): its constant's score (0: not a save code) and value.
    uint8_t look(uint16_t p, uint16_t end, uint32_t *value) {
        uint32_t w = le32(buf + p);
        uint8_t rd = (w >> 7) & 31;
        uint32_t page = 0, word0 = 0, regs = 0;       // register masks: record loads, the constant
        bool have = false, a1 = false;
        uint8_t best = 0;
        Ins i;
        // A record load in the few instructions before the lui (a lw or c.lw; 2-byte alignment, so each start
        // is tried): kept when the instructions from it run exactly up to the lui without writing its register.
        for (uint8_t k = 2; k <= BACK; k += 2) {
            if (p < k) break;
            decode(buf + p - k, i);
            if (i.kind != K_LW || !i.rd || i.len > k || (i.imm != 0 && i.imm != 0x500 && i.imm != 0x600)) continue;
            uint8_t r = i.rd;
            int16_t imm = i.imm;
            uint16_t q = p - k + i.len;
            bool ok = true;
            while (ok && q < p) { Ins j; decode(buf + q, j); ok = j.rd != r && j.kind != K_CALL && j.kind != K_STOP && j.kind != K_BRANCH; q += j.len; }
            if (!ok || q != p) continue;
            if (imm == 0) word0 |= 1u << r; else page |= 1u << r;
        }
        for (uint16_t q = p + 4; q + 4 <= end; q += i.len) {
            decode(buf + q, i);
            if (!have) {
                if (q > p + PAIR) return 0;
                if (i.kind == K_ADDI && i.rd && i.rs1 == rd) {         // addi rd2, rd, lo (rd2 may differ)
                    *value = (w & 0xFFFFF000u) + (uint32_t)(int32_t)i.imm;
                    if (!printable(*value)) return 0;
                    have = true; regs = 1u << i.rd;
                    page &= ~regs; word0 &= ~regs;
                    continue;
                }
                if (i.kind == K_CALL || i.kind == K_STOP || i.rd == rd) return 0;
            } else if (i.kind == K_BRANCH) {
                uint32_t a = 1u << i.rs1, b = 1u << i.rs2;
                uint32_t o = (regs & a) ? b : (regs & b) ? a : 0;
                if ((o & page) && best < SURE) best = SURE;
                else if ((o & word0) && best < WORD) best = WORD;
            } else if (i.kind == K_CALL || i.kind == K_STOP) {
                if ((regs & (1u << 10)) && a1 && best < CALL) best = CALL;
                break;
            }
            if (i.kind == K_LI && i.rd == 11) a1 = true;
            if (!i.rd) continue;
            uint32_t bit = 1u << i.rd;
            page &= ~bit; word0 &= ~bit;
            if (i.kind == K_MV && (regs & (1u << i.rs1))) regs |= bit;
            else regs &= ~bit;
            if (have && !regs) break;
            if (i.kind == K_LW) {
                if (i.imm == 0) word0 |= bit;
                else if (i.imm == 0x500 || i.imm == 0x600) page |= bit;
            }
        }
        return have ? best : 0;
    }

    void run(uint16_t total, bool last) {
        uint16_t p = back;
        if ((pos + p) & 1) p++;                     // instructions sit at even payload offsets
        for (; p + 4 <= total; p += 2) {
            if (!last && p + AHEAD > total) break;  // not all here yet: carry it
            // A lui (rd not 0) whose top two bytes can still give a printable code: the addi moves them by at most
            // 1, so each must be 0x20..0x7F (0.19: skips look() for addresses and most other constants, 2x faster).
            const uint8_t *b = buf + p;
            if ((b[0] & 0x7F) != 0x37 || !((b[0] >> 7) | (b[1] & 15)) || (uint8_t)(b[2] - 0x20) > 0x5F ||
                (uint8_t)(b[3] - 0x20) > 0x5F) continue;
            uint32_t v;
            uint16_t end = p + AHEAD < total ? p + AHEAD : total;
            uint8_t s = look(p, end, &v);
            if (s) add(v, s);
        }
        if (last) return;
        // p is the first unscanned offset; keep BACK bytes before it as context.
        uint16_t from = p >= BACK ? p - BACK : 0;
        back = (uint8_t)(p - from);
        carry = total - from;
        memmove(buf, buf + from, carry);
        pos += from;
    }
};

}  // namespace chgscan
