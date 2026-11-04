// VCard - a pretend FAT16 card around one plain file, for the simulator and
// host tests (CHSd, MIT). Header-only, PC side.
//
// A game's sim used to hand its data file to the code as the card itself
// (file block k at LBA k), so the FAT reader never ran off the board. With
// this the code under test sees a whole volume and goes through mount(),
// find(), runs() and read() exactly as on a card: a FAT16 "superfloppy"
// (boot sector at LBA 0, the way small cards and some PCs format them), a
// volume label and the file in the root directory, and the file in two
// pieces with a gap between, so that crossing from one run to the next is
// exercised too. The file's blocks are fetched on demand; nothing is copied.
#pragma once
#include <stdint.h>
#include <string.h>

namespace vcard {

static const uint32_t SPC = 8;               // 4 KB clusters
static const uint32_t ROOT_SECS = 32;        // 512 root entries
static const uint32_t GAP = 3;               // free clusters between the two pieces

struct Card {
    uint8_t name[11];
    uint32_t size, fc, half, n, fsz, meta;

    // path: the file (its last component, made 8.3, is the name on the
    // card); size: its length in bytes.
    void setup(const char *path, uint32_t bytes) {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        const char *dot = strrchr(base, '.');
        memset(name, ' ', 11);
        for (uint32_t i = 0; base + i != dot && base[i] && i < 8; i++) name[i] = up(base[i]);
        for (uint32_t i = 0; dot && dot[i + 1] && i < 3; i++) name[8 + i] = up(dot[i + 1]);
        size = bytes;
        fc = (size + SPC * 512 - 1) / (SPC * 512);
        half = fc / 2;
        n = fc + GAP + 1;
        if (n < 4100) n = 4100;              // FAT16 needs 4,085 clusters or more
        fsz = ((n + 2) * 2 + 511) / 512;
        meta = 1 + fsz + ROOT_SECS;
    }
    uint32_t blocks() const { return meta + n * SPC; }

    // Cluster of the file's cluster i: the first half from 2, the rest after the gap.
    uint32_t clusterOf(uint32_t i) const { return 2 + i + (i >= half && half ? GAP : 0); }
    // The other way: which of the file's clusters c is, or -1 (not the file's).
    long indexOf(uint32_t c) const {
        if (c < 2) return -1;
        uint32_t i = c - 2;
        if (half && i >= half) {
            if (i < half + GAP) return -1;
            i -= GAP;
        }
        return i < fc ? (long)i : -1;
    }

    // The block at lba into dst. Returns the file's block number when the
    // block is the file's (dst left alone: fetch it), else -1 (dst filled).
    long read(uint32_t lba, uint8_t *dst) const {
        memset(dst, 0, 512);
        if (lba == 0) {
            static const uint8_t jmp[3] = {0xEB, 0x3C, 0x90};
            memcpy(dst, jmp, 3);
            memcpy(dst + 3, "CHSD SIM", 8);
            put16(dst + 11, 512);
            dst[13] = SPC;
            put16(dst + 14, 1);              // reserved sectors
            dst[16] = 1;                     // one FAT
            put16(dst + 17, ROOT_SECS * 16);
            dst[21] = 0xF8;
            put16(dst + 22, fsz);
            put32(dst + 32, blocks());
            dst[38] = 0x29;
            memcpy(dst + 43, "CHSD SIM   FAT16   ", 19);
            put16(dst + 510, 0xAA55);
            return -1;
        }
        if (lba < 1 + fsz) {                 // the FAT
            uint32_t first = (lba - 1) * 256;
            for (uint32_t e = 0; e < 256; e++) {
                uint32_t c = first + e, v = 0;
                long i = indexOf(c);
                if (c == 0) v = 0xFFF8;
                else if (c == 1) v = 0xFFFF;
                else if (i >= 0) v = (uint32_t)i + 1 < fc ? clusterOf((uint32_t)i + 1) : 0xFFFF;
                put16(dst + 2 * e, v);
            }
            return -1;
        }
        if (lba < meta) {                    // the root directory
            if (lba == 1 + fsz) {
                memcpy(dst, "CHSD SIM   ", 11);
                dst[11] = 0x08;              // the volume label: passed over
                memcpy(dst + 32, name, 11);
                dst[32 + 11] = 0x20;
                put16(dst + 32 + 26, fc ? 2 : 0);
                put32(dst + 32 + 28, size);
            }
            return -1;
        }
        long i = indexOf(2 + (lba - meta) / SPC);
        uint32_t k = (uint32_t)i * SPC + (lba - meta) % SPC;
        return i >= 0 && k * 512 < size ? (long)k : -1;
    }

    static uint8_t up(char ch) { return (uint8_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); }
    static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
};

}  // namespace vcard
