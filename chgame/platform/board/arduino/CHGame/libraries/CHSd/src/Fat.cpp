// Fat.cpp - see Fat.h. From HypeRunner's src/sd/Fat.cpp (MIT): clean-room
// from the Microsoft FAT specification and the MBR layout, by way of the
// cuts CHWords and CHCrossword made of it. Files are looked for in the root
// directory or in a folder of it. This file runs on the PC as well: the
// simulator's card and the host tests go through it, so what is tested is
// what the board runs.
//
// Size notes (-Os, RV32): word-sized locals avoid the masking and sign
// extension that uint8_t/int8_t locals cost. Fields at odd offsets (some BPB
// ones) are read with byte loads; aligned ones straight from the 4-byte
// aligned buffer (little-endian, like FAT).
#pragma GCC optimize("Os", "no-ipa-sra")
#include "Fat.h"
#include "SdSpi.h"

namespace fat {

namespace {

struct Vol {
    uint32_t fat;            // LBA of the first FAT
    uint32_t data;           // LBA of cluster 2
    uint32_t root;           // FAT16: LBA of the fixed root region; FAT32: root cluster
    uint32_t end;            // one past the highest cluster number; 0 = not mounted
    uint32_t rootSecs;       // FAT16 root region sectors; 0 means FAT32
    uint32_t shift;          // log2(sectors per cluster)
};
Vol v;

uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
inline uint32_t u16at(const uint8_t *p) { return *(const uint16_t *)p; }   // p 2-byte aligned
inline uint32_t u32at(const uint8_t *p) { return *(const uint32_t *)p; }   // p 4-byte aligned

uint32_t clusLba(uint32_t c) { return v.data + ((c - 2) << v.shift); }

// FAT entry of cluster c: the next cluster (>= 2), 0 at the end of the
// chain, 1 for a free, bad or out-of-range link, or a negative sd error.
// `cached` names the FAT sector already in b, so a chain walk reads each
// FAT sector once.
int32_t next(uint32_t c, uint8_t *b, uint32_t &cached) {
    const bool f32 = !v.rootSecs;
    uint32_t off = c << (f32 ? 2 : 1), sec = v.fat + (off >> 9);
    if (sec != cached) {
        if (!sd::read(sec, b)) return E_READ;
        cached = sec;
    }
    const uint8_t *p = b + (off & 511);
    uint32_t n = f32 ? u32at(p) & 0x0FFFFFFFu : u16at(p);
    if (n >= (f32 ? 0x0FFFFFF8u : 0xFFF8u)) return 0;
    return n >= 2 && n < v.end ? (int32_t)n : 1;
}

// Validates the BPB in b (the volume starts at LBA base) and fills v. Only
// the first FAT is read: mirroring is assumed, as every formatter sets it.
bool bpb(const uint8_t *b, uint32_t base) {
    uint32_t spc = b[13], sh = 0, nf = b[16];
    while ((1u << sh) < spc) sh++;
    uint32_t rsvd = u16at(b + 14), rootSecs = (le16(b + 17) + 15) >> 4;
    uint32_t tot = le16(b + 19), fsz = u16at(b + 22);
    if (!tot) tot = u32at(b + 32);
    if (!fsz) fsz = u32at(b + 36);
    if (le16(b + 11) != 512 || (1u << sh) != spc || !rsvd || !nf || !fsz) return false;
    if (nf > 4 || fsz >= (1u << 24)) return false;  // keeps nf * fsz and fsz * 256 in range
    uint32_t meta = rsvd + nf * fsz + rootSecs;
    if (meta >= tot) return false;
    // FAT12 (under 4,085 clusters) is refused. FAT32 is told by its empty
    // fixed root region, which on a valid volume agrees with the spec's
    // cluster-count rule (65,525 and up); a volume where they disagree is
    // malformed, and what it yields fails the chain and pack checks.
    uint32_t n = (tot - meta) >> sh;                 // clusters
    if (n < 4085) return false;
    // The FAT must hold an entry per cluster (else a chain reads past it),
    // and a FAT32 root must be a real cluster.
    if (fsz * (rootSecs ? 256u : 128u) < n + 2) return false;
    if (!rootSecs && (u32at(b + 44) < 2 || u32at(b + 44) >= n + 2)) return false;
    v.fat = base + rsvd;
    v.data = base + meta;
    v.root = rootSecs ? v.fat + nf * fsz : u32at(b + 44);
    v.rootSecs = rootSecs;
    v.shift = sh;
    v.end = n + 2;
    return true;
}

// Finds an 11-byte short name ('?' = any character) in directory `dir` (a
// cluster, or 0 for the FAT16 root region), passing over the first `skip`
// that fit. `want` is the entry's attributes masked with ATTR_TEST: 0 for a
// plain file, ATTR_DIR for a folder; so deleted entries aside, volume
// labels, long-name parts and hidden entries never match, and neither does
// a file for a folder or the other way round, all in one test. A directory
// holds at most 65,536 entries (4,096 sectors), which bounds the walk when
// a chain loops. b is shared by the directory sectors and the FAT lookups,
// so nothing is cached here.
const uint32_t ATTR_DIR = 0x10, ATTR_TEST = ATTR_DIR | 0x08 | 0x02;   // folder, label, hidden

int32_t lookup(uint32_t dir, const uint8_t *name, File &f, uint8_t *b, uint32_t want, uint32_t skip,
               char *nameOut) {
    uint32_t sec = 0;
    for (uint32_t guard = 4096; guard--;) {
        uint32_t lba;
        if (!dir) {
            if (sec == v.rootSecs) break;              // end of the FAT16 root region
            lba = v.root + sec;
        } else {
            if (sec >> v.shift) {                      // this cluster is done
                uint32_t none = ~0u;
                int32_t n = next(dir, b, none);
                if (n < 2) return n < 0 ? n : (int32_t)E_NOTFOUND;
                dir = (uint32_t)n;
                sec = 0;
            }
            lba = clusLba(dir) + sec;
        }
        sec++;
        if (!sd::read(lba, b)) return E_READ;
        for (const uint8_t *d = b; d < b + 512; d += 32) {
            if (!d[0]) return E_NOTFOUND;                   // end-of-directory mark
            if (d[0] == 0xE5 || (d[11] & ATTR_TEST) != want) continue;   // deleted, or not the kind wanted
            uint32_t i = 0;
            while (i < 11 && (d[i] == name[i] || name[i] == '?')) i++;
            if (i < 11) continue;
            if (skip) { skip--; continue; }
            if (nameOut) for (i = 0; i < 11; i++) nameOut[i] = (char)d[i];
            // The high cluster word only exists on FAT32 (a fixed root
            // region means FAT16); FAT16 ignores it, as the PC tools do.
            f.cluster = u16at(d + 26) | (v.rootSecs ? 0u : u16at(d + 20) << 16);
            f.size = u32at(d + 28);
            return OK;
        }
    }
    return E_NOTFOUND;
}

}  // namespace

int8_t mount(uint8_t *b) {
    v.end = 0;
    uint32_t base = 0;
    for (uint32_t pass = 0;; pass++) {
        if (!sd::read(base, b)) return E_READ;
        if (u16at(b + 510) != 0xAA55) return E_NOFS;
        if (le32(b + 3) == 0x41465845u) return E_EXFAT;       // OEM name "EXFAT   "
        if ((b[0] == 0xEB || b[0] == 0xE9) && bpb(b, base)) return OK;
        if (pass) return E_NOFS;
        // LBA 0 is not a usable boot sector: read it as an MBR and take the
        // first FAT partition. With none, a type 07 one (exFAT, or NTFS)
        // gets its own error: the card needs reformatting as FAT32.
        bool other = false;
        for (const uint8_t *p = b + 446; p < b + 510; p += 16) {
            uint32_t t = p[4];
            if (t < 16 && (0x5852u >> t & 1)) { base = le32(p + 8); break; }   // 01 04 06 0B 0C 0E
            other |= t == 0x07;
        }
        if (!base) return other ? E_EXFAT : E_NOFS;
    }
}

int8_t find(const char *name, File &f, uint8_t *b) {
    if (!v.end) return E_NOTFOUND;
    return (int8_t)lookup(v.rootSecs ? 0 : v.root, (const uint8_t *)name, f, b, 0, 0, nullptr);
}

int8_t folder(const char *name, File &dir, uint8_t *b) {
    if (!v.end) return E_NOTFOUND;
    int32_t rc = lookup(v.rootSecs ? 0 : v.root, (const uint8_t *)name, dir, b, ATTR_DIR, 0, nullptr);
    // (A folder's first cluster is never 0: that would be the root.)
    if (!rc && dir.cluster < 2) rc = E_NOTFOUND;
    return (int8_t)rc;
}

int8_t match(const File &dir, const char *pattern, uint8_t skip, File &f, char *nameOut, uint8_t *b) {
    if (!v.end) return E_NOTFOUND;
    return (int8_t)lookup(dir.cluster, (const uint8_t *)pattern, f, b, 0, skip, nameOut);
}

int8_t root(File &dir) {
    dir.cluster = v.rootSecs ? 0 : v.root;
    dir.size = 0;
    return v.end ? (int8_t)OK : (int8_t)E_NOTFOUND;
}

// lookup()'s walk, handing every visible entry to fn. Kept apart from it so
// the games, which never list, keep their code as it was.
int8_t list(const File &d, ListFn fn, void *ctx, uint8_t *b) {
    if (!v.end) return E_NOTFOUND;
    uint32_t dir = d.cluster, sec = 0;
    for (uint32_t guard = 4096; guard--;) {
        uint32_t lba;
        if (!dir) {
            if (sec == v.rootSecs) break;
            lba = v.root + sec;
        } else {
            if (sec >> v.shift) {
                uint32_t none = ~0u;
                int32_t n = next(dir, b, none);
                if (n < 2) return n < 0 ? (int8_t)n : (int8_t)OK;
                dir = (uint32_t)n;
                sec = 0;
            }
            lba = clusLba(dir) + sec;
        }
        sec++;
        if (!sd::read(lba, b)) return E_READ;
        for (const uint8_t *e = b; e < b + 512; e += 32) {
            if (!e[0]) return OK;
            if (e[0] == 0xE5 || e[0] == '.' || (e[11] & (ATTR_TEST & ~ATTR_DIR))) continue;
            File f;
            f.cluster = u16at(e + 26) | (v.rootSecs ? 0u : u16at(e + 20) << 16);
            f.size = u32at(e + 28);
            if (!fn((const char *)e, f, (e[11] & ATTR_DIR) != 0, ctx)) return OK;
        }
    }
    return OK;
}

int8_t runs(const File &f, Run *out, uint8_t maxRuns, uint8_t *b) {
    if (maxRuns > 127) maxRuns = 127;                 // the count comes back in an int8_t
    uint32_t left = (f.size >> 9) + ((f.size & 511) != 0), c = f.cluster, cached = ~0u;
    const uint32_t spc = 1u << v.shift;
    uint32_t n = 0;
    while (left) {
        if (c < 2 || c >= v.end) return E_CHAIN;      // also 0/1 from next(), or not mounted
        uint32_t lba = clusLba(c), k = left < spc ? left : spc;
        Run *r = out + n;
        if (n && r[-1].lba + r[-1].blocks == lba) {
            r[-1].blocks += k;
        } else {
            if (n == maxRuns) return E_FRAG;
            r->lba = lba;
            r->blocks = k;
            n++;
        }
        left -= k;
        int32_t nx = next(c, b, cached);
        if (nx < 0) return (int8_t)nx;
        if (!left) return nx ? (int8_t)E_CHAIN : (int8_t)n;   // the chain must end exactly here
        c = (uint32_t)nx;
    }
    return 0;
}

// (Passing the reason for a failure on would cost some 40 B of flash in
// every game that only needs to know whether the file is there.)
uint8_t open(const char *name, Run *out, uint8_t maxRuns, uint8_t *b) {
    File f;
    if (!sd::init() || mount(b) || find(name, f, b)) return 0;
    int8_t n = runs(f, out, maxRuns, b);
    return n > 0 ? (uint8_t)n : 0;
}

bool read(const Run *r, uint32_t n, uint32_t k, uint8_t *dst) {
    for (; n; n--, r++) {
        if (k < r->blocks) return sd::read(r->lba + k, dst);
        k -= r->blocks;
    }
    return false;
}

}  // namespace fat
