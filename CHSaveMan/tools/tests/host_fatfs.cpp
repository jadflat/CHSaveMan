// Host test: the board's card code (src/sd/sdio_dev.cpp) and FatFs (src/fatfs/ff.cpp, the board's ffconf.h)
// on a RAM disk formatted FAT16 and FAT32 (with an MBR, as cards come). Run by `./cl test` (tools/game.py).
//   - mount results: an empty slot (NO_CARD), a card that fails init (IO: CARD ERROR), no FAT volume (NOT_FAT)
//   - SAVES.BAK / .BK1 / .UND, SAVES/<CODE>.SAV / .SV1 (the SAVES folder is made), read back, srcScan's list
//   - power cuts: the disk image after every sector write of a backup, a .SAV write, an undo write and a
//     SAVENAME.TXT append; each must still read as the old or the new file, and the next write must work
//   - failed writes (a sector write that errors at each point): IO, and the old file is still readable
//   - SAVENAME.TXT append and search, IGNORE.TXT replace, GAMELIST.TXT write and read
//   - the GAMES scan: every game's code and path, two sub-folder levels, deep / junk / not-CHG1 files,
//     codes on 512-byte and SCAN_STEP edges, a remount ending a scan; sectors read per scanNext call
//   - a frame-time model of the app's scanStep (CHSaveMan.ino, sdio::scanRoom) with GAMELIST.TXT written as it goes
// Prints "ok"/"FAIL" lines and "ALL OK" (exit 0) or "n FAILED" (exit 1).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "../../sdio.h"
#include "../../src/fatfs/ff.h"          // after the C++ headers (zig's libc++ and FatFs's macros)
#include "../../src/fatfs/diskio.h"

static int fails = 0, oks = 0;
static void check(bool ok, const char *what, const std::string &more = "") {
    if (ok) oks++; else fails++;
    if (!ok) printf("FAIL %s %s\n", what, more.c_str());
}

// ---- the RAM disk ----

static std::vector<uint8_t> img;
static bool present = true;           // false: an empty slot
static uint8_t initErr = 1;           // why init failed when !present: 1 no answer to CMD0 (NO CARD), else a card error
static long nWrites = 0, failAt = -1; // sector writes so far; failAt >= 0: that write and later ones fail
struct WLog { uint32_t lba; uint8_t d[512]; };
static bool logOn = false;
static std::vector<WLog> wlog;
static uint64_t rdSec = 0;

extern "C" {
uint8_t chsd_init_err = 0;
uint8_t chsd_stream = 0, chsd_fail = 0;
void chsd_end(void) {}
DSTATUS disk_initialize(BYTE d) {
    if (d) return STA_NOINIT;
    if (!present) { chsd_init_err = initErr; return STA_NOINIT; }
    chsd_init_err = 0;
    return 0;
}
DSTATUS disk_status(BYTE d) { return (d || !present) ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE d, BYTE *buf, LBA_t s, UINT n) {
    if (d || !present) return RES_NOTRDY;
    if ((uint64_t)(s + n) * 512 > img.size()) return RES_PARERR;
    memcpy(buf, img.data() + (size_t)s * 512, (size_t)n * 512);
    rdSec += n;
    return RES_OK;
}
DRESULT disk_write(BYTE d, const BYTE *buf, LBA_t s, UINT n) {
    if (d || !present) return RES_NOTRDY;
    if ((uint64_t)(s + n) * 512 > img.size()) return RES_PARERR;
    for (UINT i = 0; i < n; i++) {
        if (failAt >= 0 && nWrites >= failAt) { chsd_fail = 1; return RES_ERROR; }
        nWrites++;
        memcpy(img.data() + (size_t)(s + i) * 512, buf + 512 * i, 512);
        if (logOn) { WLog w; w.lba = s + i; memcpy(w.d, buf + 512 * i, 512); wlog.push_back(w); }
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE d, BYTE cmd, void *buf) {
    if (d || !present) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buf = (LBA_t)(img.size() / 512); return RES_OK;
        case GET_SECTOR_SIZE: *(WORD *)buf = 512; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD *)buf = 1; return RES_OK;
        default: return RES_PARERR;
    }
}
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// FAT16, no partition table (a "superfloppy"): 16 MB with 2 KB clusters, or 160 MB with 32 KB clusters (as
// card formatters choose; a folder then rarely needs a FAT read).
static void formatFat16Size(uint32_t total, uint32_t spc) {
    const uint32_t rsvd = 4, rootSec = 32;
    img.assign((size_t)total * 512, 0); sdio::forget();
    uint32_t fatsz = 1, clus = 0;
    for (;;) {
        clus = (total - rsvd - 2 * fatsz - rootSec) / spc;
        uint32_t need = ((clus + 2) * 2 + 511) / 512;
        if (need <= fatsz) break;
        fatsz = need;
    }
    uint8_t *b = img.data();
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90; memcpy(b + 3, "MSDOS5.0", 8);
    put16(b + 11, 512); b[13] = spc; put16(b + 14, rsvd); b[16] = 2; put16(b + 17, 512); put16(b + 19, total < 65536 ? total : 0); put32(b + 32, total < 65536 ? 0 : total);
    b[21] = 0xF8; put16(b + 22, fatsz); put16(b + 24, 63); put16(b + 26, 255);
    b[36] = 0x80; b[38] = 0x29; put32(b + 39, 0x12345678); memcpy(b + 43, "HOSTFAT16  ", 11); memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    for (int f = 0; f < 2; f++) { uint8_t *t = b + (rsvd + f * fatsz) * 512; t[0] = 0xF8; t[1] = t[2] = t[3] = 0xFF; }
}
static void formatFat16() { formatFat16Size(32768, 4); }
static void formatFat16Big() { formatFat16Size(327680, 64); }

// FAT32: 40 MB card, MBR, the partition at sector 2048, 1 sector a cluster (as tools/mkcard.py).
static void formatFat32() {
    const uint32_t size = 81920, start = 2048, total = size - start, rsvd = 32;
    img.assign((size_t)size * 512, 0); sdio::forget();
    uint32_t fatsz = 1, clus = 0;
    for (;;) {
        clus = total - rsvd - 2 * fatsz;
        uint32_t need = ((clus + 2) * 4 + 511) / 512;
        if (need <= fatsz) break;
        fatsz = need;
    }
    uint8_t *m = img.data();
    m[446] = 0x00; m[450] = 0x0C; put32(m + 454, start); put32(m + 458, total); m[510] = 0x55; m[511] = 0xAA;
    uint8_t *b = img.data() + (size_t)start * 512;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90; memcpy(b + 3, "MSDOS5.0", 8);
    put16(b + 11, 512); b[13] = 1; put16(b + 14, rsvd); b[16] = 2; b[21] = 0xF8; put16(b + 24, 63); put16(b + 26, 255);
    put32(b + 28, start); put32(b + 32, total); put32(b + 36, fatsz); put32(b + 44, 2); put16(b + 48, 1); put16(b + 50, 6);
    b[64] = 0x80; b[66] = 0x29; put32(b + 67, 0x12345678); memcpy(b + 71, "HOSTFAT32  ", 11); memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    uint8_t *fi = b + 512;
    put32(fi, 0x41615252); put32(fi + 484, 0x61417272); put32(fi + 488, 0xFFFFFFFF); put32(fi + 492, 3);
    fi[510] = 0x55; fi[511] = 0xAA;
    memcpy(b + 6 * 512, b, 1024);
    for (int f = 0; f < 2; f++) {
        uint8_t *t = b + (rsvd + f * fatsz) * 512;
        put32(t, 0x0FFFFFF8); put32(t + 4, 0x0FFFFFFF); put32(t + 8, 0x0FFFFFFF);
    }
}

// ---- files through FatFs (the test's own mount; sdio mounts again on each call) ----

static FATFS tfs;
static bool tmount() { return f_mount(&tfs, "", 1) == FR_OK; }
static bool putFile(const char *path, const std::vector<uint8_t> &d) {
    if (!tmount()) return false;
    std::string p = path;
    for (size_t i = p.find('/'); i != std::string::npos; i = p.find('/', i + 1)) f_mkdir(p.substr(0, i).c_str());
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT w = 0;
    bool ok = d.empty() || (f_write(&f, d.data(), (UINT)d.size(), &w) == FR_OK && w == d.size());
    return f_close(&f) == FR_OK && ok;
}
static bool getFile(const char *path, std::vector<uint8_t> &d) {
    d.clear();
    if (!tmount()) return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    d.resize(f_size(&f));
    UINT r = 0;
    bool ok = d.empty() || (f_read(&f, d.data(), (UINT)d.size(), &r) == FR_OK && r == d.size());
    f_close(&f);
    return ok;
}
static bool exists(const char *path) { FILINFO fi; return tmount() && f_stat(path, &fi) == FR_OK; }
static std::vector<uint8_t> bytes(const char *s) { return std::vector<uint8_t>(s, s + strlen(s)); }
static std::string str(const std::vector<uint8_t> &d) { return std::string(d.begin(), d.end()); }

// Two pages with a recognisable record each (seq in byte 6).
static void pages(uint8_t *p, const char *a, uint8_t seqA, const char *b, uint8_t seqB) {
    memset(p, 0xFF, 512);
    memcpy(p, a, 4); p[4] = 1; p[5] = 0; p[6] = seqA;
    memcpy(p + 256, b, 4); p[256 + 4] = 1; p[256 + 5] = 0; p[256 + 6] = seqB;
    for (int i = 8; i < 48; i++) { p[i] = (uint8_t)(i * seqA); p[256 + i] = (uint8_t)(i * seqB); }
}

// ---- mount results ----

static void testMount(void (*format)()) {
    uint8_t p[512];
    present = false; initErr = 1;
    check(sdio::readRec("SAVES.BAK", "SAVES.TMP", p, 2) == sdio::NO_CARD, "empty slot: NO_CARD");
    sdio::Scan s;
    check(sdio::scanBegin(s) == sdio::NO_CARD, "empty slot: the scan says NO_CARD");
    initErr = 0xFF;
    check(sdio::readRec("SAVES.BAK", "SAVES.TMP", p, 2) == sdio::IO, "a card that fails init: IO (CARD ERROR)");
    pages(p, "CHAA", 1, "CHBB", 2);
    check(sdio::backup(p) == sdio::IO, "a card that fails init: the backup is IO");
    present = true;
    img.assign(16 << 20, 0); sdio::forget();
    check(sdio::readRec("SAVES.BAK", "SAVES.TMP", p, 2) == sdio::NOT_FAT, "a card with no FAT volume: NOT_FAT");
    check(sdio::backup(p) == sdio::NOT_FAT, "no FAT volume: the backup is NOT_FAT");
    format();
    check(sdio::readRec("SAVES.BAK", "SAVES.TMP", p, 2) == sdio::NO_FILE, "a fresh card: NO_FILE");
    check(sdio::scanBegin(s) == sdio::NO_GAMES, "a fresh card: NO_GAMES");
    sdio::scanEnd(s);
}

// ---- record files ----

static std::vector<std::string> listed;
static void srcCb(const char *name, uint8_t n, sdio::Result r, const uint8_t *pg) {
    char b[64];
    snprintf(b, sizeof b, "%s:%u:%u:%.4s:%u", name, n, r, r == sdio::OK ? (const char *)pg : "----", r == sdio::OK ? pg[6] : 0);
    listed.push_back(b);
}
static std::string srcList() {
    listed.clear();
    uint8_t p[512];
    sdio::Result r = sdio::srcScan(p, srcCb);
    std::string o = r == sdio::OK ? "" : "ERR";
    for (auto &x : listed) o += (o.empty() ? "" : " ") + x;
    return o;
}

static void testRecords() {
    uint8_t p[512], q[512];
    pages(p, "CHAA", 5, "CHBB", 6);
    check(sdio::backup(p) == sdio::OK, "backup to a card with no SAVES.BAK");
    check(sdio::restore(q) == sdio::OK && !memcmp(p, q, 512), "SAVES.BAK reads back the same 512 bytes");
    std::vector<uint8_t> d;
    check(getFile("SAVES.BAK", d) && d.size() == 528 && !memcmp(d.data(), "CHSVBK01", 8) && d[12] == 2 && d[14] == 1,
          "SAVES.BAK is 528 B: tag, 2 pages, 1 set");
    check(!exists("SAVES.TMP") && !exists("SAVES.BK1"), "no SAVES.TMP left, no SAVES.BK1 yet");
    std::vector<uint8_t> first = d;
    pages(p, "CHAA", 7, "CHBB", 8);
    check(sdio::backup(p) == sdio::OK, "a second backup");
    check(getFile("SAVES.BK1", d) && d == first, "the replaced SAVES.BAK is SAVES.BK1, byte for byte");
    check(sdio::restore(q) == sdio::OK && q[6] == 7 && q[256 + 6] == 8, "SAVES.BAK is the new one (#7 #8)");
    // A bad SAVES.BAK never replaces a good SAVES.BK1.
    std::vector<uint8_t> bk1;
    getFile("SAVES.BK1", bk1);
    getFile("SAVES.BAK", d);
    d[100] ^= 0xFF;
    putFile("SAVES.BAK", d);
    check(sdio::restore(q) == sdio::BAD_FILE, "a damaged SAVES.BAK reads BAD_FILE");
    pages(p, "CHAA", 9, "CHBB", 10);
    check(sdio::backup(p) == sdio::OK && getFile("SAVES.BK1", d) && d == bk1, "a damaged SAVES.BAK is dropped, SAVES.BK1 kept");
    // SAVES.UND (no bk1), the SAVES folder and the .SAV / .SV1 pair.
    check(sdio::writeRec("SAVES.UND", "SAVES.UTM", p, 2, nullptr) == sdio::OK && exists("SAVES.UND") && !exists("SAVES.UTM"),
          "SAVES.UND written (no .UTM left)");
    check(!exists("SAVES"), "no SAVES folder before the first .SAV");
    uint8_t g[256];
    memcpy(g, p, 256);
    check(sdio::writeRec("SAVES/CHAA.SAV", "SAVES/CHAA.TMP", g, 1, "SAVES/CHAA.SV1") == sdio::OK, "SAVES/CHAA.SAV written");
    FILINFO fi;
    check(tmount() && f_stat("SAVES", &fi) == FR_OK && (fi.fattrib & AM_DIR), "the SAVES folder was made");
    check(getFile("SAVES/CHAA.SAV", d) && d.size() == 272 && d[12] == 1, "CHAA.SAV is 272 B, 1 page");
    check(!exists("SAVES/CHAA.SV1"), "no .SV1 after the first .SAV");
    std::vector<uint8_t> sav1 = d;
    g[6] = 11;
    check(sdio::writeRec("SAVES/CHAA.SAV", "SAVES/CHAA.TMP", g, 1, "SAVES/CHAA.SV1") == sdio::OK, "SAVES/CHAA.SAV replaced");
    check(getFile("SAVES/CHAA.SV1", d) && d == sav1, "the replaced .SAV is CHAA.SV1, byte for byte");
    uint8_t h[256];
    check(sdio::readRec("SAVES/CHAA.SAV", "SAVES/CHAA.TMP", h, 1) == sdio::OK && h[6] == 11, "CHAA.SAV reads #11");
    check(sdio::readRec("SAVES/CHAA.SAV", "SAVES/CHAA.TMP", h, 2) == sdio::BAD_FILE, "a .SAV read as two pages: BAD_FILE");
    memcpy(g, "\x61\x62\x2D\x63", 4);
    check(sdio::writeRec("SAVES/61622D63.SAV", "SAVES/61622D63.TMP", g, 1, "SAVES/61622D63.SV1") == sdio::OK,
          "a hex-named .SAV (8.3 name) written");
    std::string l = srcList();
    bool all = listed.size() == 5 && l.rfind("SAVES.BAK:2:0:CHAA:9 SAVES.BK1:2:0:CHAA:5 SAVES.UND:2:0:CHAA:9 ", 0) == 0;
    for (const char *x : { "CHAA.SAV:1:0:CHAA:11", "61622D63.SAV:1:0:ab-c:11" }) all &= l.find(x) != std::string::npos;
    check(all, "srcScan lists BAK, BK1, UND, then every .SAV (not .SV1)", l);
    // A .SAV write cut off after the old one became .SV1: only CHAA.TMP is left; the list still shows the save.
    tmount();
    f_unlink("SAVES/CHAA.SAV");
    getFile("SAVES/CHAA.SV1", d);
    putFile("SAVES/CHAA.TMP", d);
    l = srcList();
    check(l.find("CHAA.SAV:1:0:CHAA:11") == std::string::npos && l.find("CHAA.SAV:1:0:CHAA:") != std::string::npos,
          "a lone SAVES/CHAA.TMP is listed as CHAA.SAV (its record)", l);
    check(sdio::readRec("SAVES/CHAA.SAV", "SAVES/CHAA.TMP", h, 1) == sdio::OK, "a lone .TMP reads as the .SAV");
    g[6] = 12;
    memcpy(g, "CHAA", 4);
    check(sdio::writeRec("SAVES/CHAA.SAV", "SAVES/CHAA.TMP", g, 1, "SAVES/CHAA.SV1") == sdio::OK && !exists("SAVES/CHAA.TMP") &&
          getFile("SAVES/CHAA.SV1", d) && d == sav1, "the next .SAV write promotes the .TMP (it becomes the .SV1)");
    // srcScan with the bare minimum: no SAVES.BAK, no folder.
    formatFat16();
    check(srcList() == "SAVES.BAK:2:3:----:0", "an empty card: the list has SAVES.BAK as NO_FILE", srcList());
}

// ---- power cuts and failed writes ----

static std::vector<uint8_t> base;
static void stateAfter(size_t k) {            // the card as a power cut after k logged sector writes leaves it
    img = base; sdio::forget();
    for (size_t i = 0; i < k; i++) memcpy(img.data() + (size_t)wlog[i].lba * 512, wlog[i].d, 512);
}

static void testCuts(const char *what, const char *fin, const char *tmp, uint8_t n, const char *bk1) {
    uint8_t oldp[512], newp[512], q[512], nxt[512];
    pages(oldp, "CHAA", 20, "CHBB", 21);
    pages(newp, "CHAA", 30, "CHBB", 31);
    pages(nxt, "CHAA", 40, "CHBB", 41);
    check(sdio::writeRec(fin, tmp, oldp, n, bk1) == sdio::OK, what, "setup write");
    base = img;
    wlog.clear(); logOn = true;
    sdio::Result r = sdio::writeRec(fin, tmp, newp, n, bk1);
    logOn = false;
    std::vector<uint8_t> done = img;
    check(r == sdio::OK && wlog.size() > 3, what, "the logged write");
    int bad = 0, viaTmp = 0, olds = 0, news = 0, badNext = 0;
    for (size_t k = 0; k <= wlog.size(); k++) {
        stateAfter(k);
        r = sdio::readRec(fin, tmp, q, n);
        bool isOld = r == sdio::OK && !memcmp(q, oldp, 256 * n), isNew = r == sdio::OK && !memcmp(q, newp, 256 * n);
        if (isOld) olds++;
        if (isNew) news++;
        if (!isOld && !isNew) { bad++; printf("     %s: cut after write %zu of %zu: result %u\n", what, k, wlog.size(), r); }
        if (r == sdio::OK && !exists(fin) && exists(tmp)) viaTmp++;
        if (sdio::writeRec(fin, tmp, nxt, n, bk1) != sdio::OK || sdio::readRec(fin, tmp, q, n) != sdio::OK ||
            memcmp(q, nxt, 256 * n) || exists(tmp)) badNext++;
    }
    char m[160];
    snprintf(m, sizeof m, "%zu cut points: %d old, %d new, %d only the .TMP", wlog.size() + 1, olds, news, viaTmp);
    check(!bad, what, m);
    check(viaTmp > 0, what, std::string("a cut leaves the .TMP alone and it is read: ") + m);
    check(!badNext, what, "after every cut the next write works and leaves no .TMP");
    printf("ok   %s: %s\n", what, m);
    // A sector write that fails at each point: IO, and the file still reads as the old or the new one.
    int failBad = 0, failOk = 0;
    for (size_t k = 0; k < wlog.size(); k++) {
        img = base; sdio::forget();
        nWrites = 0; failAt = (long)k;
        r = sdio::writeRec(fin, tmp, newp, n, bk1);
        failAt = -1;
        sdio::Result rr = sdio::readRec(fin, tmp, q, n);
        bool good = rr == sdio::OK && (!memcmp(q, oldp, 256 * n) || !memcmp(q, newp, 256 * n));
        if (r == sdio::OK) failOk++;
        if (!good) failBad++;
    }
    check(!failBad, what, "a failing sector write never loses the file");
    check(!failOk, what, "a failing sector write is reported (IO)");
    img = done; sdio::forget();
}

static void testAppendCut() {
    const char *old = "# names\nCHIX=MY CHIX\n";
    putFile("SAVENAME.TXT", bytes(old));
    base = img;
    wlog.clear(); logOn = true;
    const char *add = "QQQA=QUUX ALPHA\nQQQB=QUUX BETA";
    check(sdio::namesAppend(add, (uint16_t)strlen(add)) == sdio::OK, "SAVENAME.TXT append");
    logOn = false;
    std::vector<uint8_t> d;
    getFile("SAVENAME.TXT", d);
    check(str(d) == std::string(old) + "\n" + add, "the append adds a line break and the lines", str(d));
    int bad = 0;
    for (size_t k = 0; k <= wlog.size(); k++) {
        stateAfter(k);
        if (!getFile("SAVENAME.TXT", d) || str(d).compare(0, strlen(old), old)) bad++;
    }
    check(!bad, "SAVENAME.TXT append cut at every write: the old lines are intact");
    int failBad = 0;
    for (size_t k = 0; k < wlog.size(); k++) {
        img = base; sdio::forget(); nWrites = 0; failAt = (long)k;
        sdio::Result r = sdio::namesAppend(add, (uint16_t)strlen(add));
        failAt = -1;
        if (r != sdio::IO || !getFile("SAVENAME.TXT", d) || str(d).compare(0, strlen(old), old)) failBad++;
    }
    check(!failBad, "a failed SAVENAME.TXT append is IO and keeps the old lines");
    img = base; sdio::forget();
    uint32_t found = 0;
    const char codes[] = "CHIXQQQAZZZZ";
    check(sdio::namesFind(codes, 4, 3, 7, &found) == sdio::OK && found == 1, "namesFind before the append: CHIX only");
    sdio::namesAppend(add, (uint16_t)strlen(add));
    check(sdio::namesFind(codes, 4, 3, 7, &found) == sdio::OK && found == 3, "namesFind after it: CHIX and QQQA");
    printf("ok   SAVENAME.TXT append: %zu cut points and failed writes keep the old lines\n", wlog.size() + 1);
}

static void testText() {
    // SAVENAME.TMP alone (a cut-off write): read in its place, and promoted by the next append.
    tmount(); f_unlink("SAVENAME.TXT");
    putFile("SAVENAME.TMP", bytes("ZZZA=FROM TMP\n"));
    char b[64]; uint16_t len = 0; bool more = false;
    check(sdio::readText("SAVENAME.TXT", "SAVENAME.TMP", b, sizeof b, &len, &more) == sdio::OK && len == 14,
          "SAVENAME.TMP read when SAVENAME.TXT is missing");
    check(sdio::namesAppend("ZZZB=NEXT", 9) == sdio::OK && !exists("SAVENAME.TMP"), "an append promotes SAVENAME.TMP first");
    std::vector<uint8_t> d;
    getFile("SAVENAME.TXT", d);
    check(str(d) == "ZZZA=FROM TMP\n\nZZZB=NEXT", "the promoted file keeps its lines", str(d));
    // A file over the read cap: whole lines of its start, more set.
    std::string big;
    for (int i = 0; i < 20; i++) big += "LINE" + std::to_string(i) + "=XXXXXXXXXX\n";
    putFile("IGNORE.TXT", bytes(big.c_str()));
    check(sdio::readText("IGNORE.TXT", "IGNORE.TMP", b, sizeof b, &len, &more) == sdio::OK && more && len < 64 && b[len - 1] == 10,
          "a text file over the cap: whole lines, more");
    check(sdio::writeText("IGNORE.TXT", "IGNORE.TMP", "A.CHG\n", 6) == sdio::OK && getFile("IGNORE.TXT", d) && str(d) == "A.CHG\n" &&
          !exists("IGNORE.TMP"), "writeText replaces IGNORE.TXT (no .TMP left)");
    check(sdio::readText("NONE.TXT", nullptr, b, sizeof b, &len, &more) == sdio::NO_FILE, "a missing text file: NO_FILE");
    // GAMELIST.TXT / .CSV: written in one go (no remount), read back with fileFeed / listFeed.
    sdio::readRec("SAVES.BAK", "SAVES.TMP", (uint8_t *)b, 0);   // a mount, as a scan starts with one
    check(sdio::listBegin("GAMELIST.CSV") == sdio::OK && sdio::listAdd("a,b\r\n", 5) == sdio::OK && sdio::listAdd("c,d\r\n", 5) == sdio::OK,
          "GAMELIST.CSV written line by line");
    sdio::listEnd();
    static std::string fed;
    fed.clear();
    check(sdio::fileFeed("GAMELIST.CSV", [](char c) { fed += c; }) == sdio::OK && fed == "a,b\r\nc,d\r\n", "GAMELIST.CSV reads back", fed);
    check(sdio::fileFeed("NOPE.TXT", [](char c) { fed += c; }) == sdio::NO_FILE, "fileFeed of a missing file: NO_FILE");
}

// ---- the GAMES scan ----

static std::vector<uint8_t> callBytes(const char *code) {
    uint32_t v = (uint8_t)code[0] | (uint8_t)code[1] << 8 | (uint8_t)code[2] << 16 | (uint32_t)(uint8_t)code[3] << 24;
    uint32_t hi = (v + 0x800) & 0xFFFFF000u, lo = (v - hi) & 0xFFF;
    uint32_t w[3] = { hi | 10 << 7 | 0x37, lo << 20 | 10 << 15 | 10 << 7 | 0x13, 0x100u << 20 | 1 << 7 | 0x6F };
    std::vector<uint8_t> o(14);
    put32(&o[0], w[0]); put16(&o[4], 0x4585); put32(&o[6], w[1]); put32(&o[10], w[2]);
    return o;
}
// A .CHG: 512-byte header ("CHG1", payload length at 0x10, title at 0x20), the payload of `len` bytes with a
// save call for `code` at `at` (none for "-").
static std::vector<uint8_t> chgFile(const char *title, const char *code, uint32_t len, uint32_t at) {
    std::vector<uint8_t> d(512 + len, 0);
    memcpy(&d[0], "CHG1", 4);
    put32(&d[0x10], len);
    memcpy(&d[0x20], title, strlen(title));
    if (strcmp(code, "-")) { auto c = callBytes(code); memcpy(&d[512 + at], c.data(), c.size()); }
    return d;
}

struct Want { const char *path, *code, *title; uint32_t len, at; };
static const Want GAMES[] = {
    { "SMALL.CHG", "SMLA", "Small", 600, 40 },
    { "EDGE512.CHG", "EDGA", "Edge 512", 3000, 506 },            // the call crosses the first 512-byte chunk
    { "EDGESTEP.CHG", "EDGB", "Edge Step", 9000, sdio::SCAN_STEP - 6 },   // ... the first scanNext's read
    { "EDGE4K.CHG", "EDGC", "Edge 4K", 12000, 4090 },
    { "LATE.CHG", "LATE", "Late Code", 300000, 299000 },          // 300 KB: many calls, the code near the end
    { "NOSAVE.CHG", "-", "No Save", 5000, 0 },
    { "SUB/INSUB.CHG", "INSB", "In Sub", 40000, 20000 },
    { "OTHER/CARDS/DEEP.CHG", "DEEP", "Deep Game", 70000, 65536 },
    { "OTHER/CARDS/DEEP2.CHG", "DEP2", "Deep Two", 2048, 2000 },
};

static std::vector<uint32_t> callSec;     // sectors each scanNext call read

static void testScan(void (*format)(), const char *label) {
    format();
    for (const Want &w : GAMES) {
        std::string p = std::string("GAMES/") + w.path;
        check(putFile(p.c_str(), chgFile(w.title, w.code, w.len, w.at)), "make a game file", p);
    }
    putFile("GAMES/A/B/C/TOODEEP.CHG", chgFile("Too Deep", "TDEP", 1000, 10));   // 3 deep: counted, not read
    putFile("GAMES/JUNK.CHG", std::vector<uint8_t>(600, 'X'));                   // not CHG1: counted as bad
    putFile("GAMES/_QUUX~1.CHG", std::vector<uint8_t>(4096, 0));                 // macOS "._QUUX.CHG" as 8.3: not counted
    putFile("GAMES/TINY.CHG", bytes("CHG1"));                                    // shorter than a header: passed over
    putFile("GAMES/COVER.PIC", std::vector<uint8_t>(2000, 1));                   // not a .CHG
    { auto d = chgFile("Cut Short", "CUTS", 100000, 10); d.resize(5000); putFile("GAMES/SHORT.CHG", d); }  // length past the end: bad

    sdio::Scan s;
    sdio::ScanItem it;
    check(sdio::scanBegin(s) == sdio::OK, "scanBegin on the GAMES tree");
    std::vector<std::string> got;
    callSec.clear();
    sdio::Result r;
    int calls = 0;
    for (;;) {
        uint64_t r0 = rdSec;
        r = sdio::scanNext(s, it);
        callSec.push_back((uint32_t)(rdSec - r0));
        calls++;
        if (getenv("HOSTFAT_V")) printf("     call %d: %u sectors, result %u\n", calls, callSec.back(), r);
        if (r == sdio::OK) {
            char c[8] = "-";
            if (it.nCodes) { memcpy(c, it.codes[0], 4); c[4] = 0; }
            got.push_back(std::string(it.path) + " " + c + (it.nCodes > 1 ? "+" : "") + " " + it.title);
        } else if (r != sdio::BUSY) break;
        if (calls > 100000) break;
    }
    sdio::scanEnd(s);
    check(r == sdio::DONE, "the scan ends DONE");
    for (const Want &w : GAMES) {
        std::string up = w.path;
        for (char &ch : up) if (ch >= 'a' && ch <= 'z') ch -= 32;
        std::string want = up + " " + w.code + " " + w.title;
        bool found = false;
        for (auto &g : got) found |= g == want;
        check(found, "the scan names every game", want);
    }
    check(got.size() == sizeof GAMES / sizeof GAMES[0], "the scan read every game and nothing else", std::to_string(got.size()));
    check(s.fileIndex == got.size() && s.skipDirs == 1 && s.skipFiles == 1 && s.badFiles == 2,
          "counts: games, 1 folder too deep (1 file), 2 not CHG1 (junk, cut short; ._ and tiny not counted)",
          std::to_string(s.fileIndex) + " " + std::to_string(s.skipDirs) + " " + std::to_string(s.skipFiles) + " " + std::to_string(s.badFiles));
    uint32_t mx = 0;
    for (uint32_t x : callSec) mx = x > mx ? x : mx;
    printf("ok   scan (%s): %zu games, %d scanNext calls, at most %u sectors a call (SCAN_STEP %u)\n", label, got.size(), calls, mx,
           sdio::SCAN_STEP);
    std::vector<uint32_t> keep = callSec;
    // A remount (any other card call) during a scan ends it with IO, as the app's comments say.
    check(sdio::scanBegin(s) == sdio::OK && sdio::scanNext(s, it) == sdio::BUSY, "a second scan starts");
    uint8_t p[512];
    sdio::restore(p);                      // 0.20: the same card is not mounted again, so the scan goes on
    r = sdio::scanNext(s, it);
    check(r == sdio::BUSY || r == sdio::OK, "a card call during a scan (same card, no remount) leaves it running", std::to_string(r));
    sdio::forget();                        // a remount still ends it
    sdio::restore(p);
    for (int i = 0; i < 100 && r == sdio::BUSY; i++) r = sdio::scanNext(s, it);
    check(r == sdio::IO || r == sdio::OK, "a remount during a scan ends it (IO) or the scan had finished its call", std::to_string(r));
    sdio::scanEnd(s);
    // A card pulled mid-scan: IO.
    check(sdio::scanBegin(s) == sdio::OK, "a third scan starts");
    sdio::scanNext(s, it);
    present = false;
    r = sdio::scanNext(s, it);
    for (int i = 0; i < 100 && r == sdio::BUSY; i++) r = sdio::scanNext(s, it);
    present = true;
    check(r == sdio::IO, "a card pulled mid-scan: the scan stops with IO", std::to_string(r));
    sdio::scanEnd(s);
    callSec = keep;
}

// ---- the frame-time model of CHSaveMan.ino's scanStep (0.19: sdio::scanRoom) ----
// The board measured (0.18, 78 games on a real card): a sector read 0.37 ms, the scanner 0.9 ms a sector (0.19's
// prefilter: about half), a .CHG open with its header up to 9.6 ms, a GAMELIST.TXT line up to 5 ms. The model
// runs the app's loop on a clock made of the calls, sectors read and sectors written so far: scanRoom says whether
// `us` more fits in the frame's SCAN_BUDGET. Checked: every frame stays within the budget, the scan still names
// every game, WAIT ends frames, no scanNext call writes (listAdd writes its own line), GAMELIST.TXT reads back.
static const uint64_t T_CALL = 400, T_SEC = 800, T_WR = 2000;
static const uint32_t SCAN_BUDGET = 18000, LINE_US = 8000;   // as CHSaveMan.ino
static uint64_t simCalls = 0, frameT = 0;
static uint64_t simNow() { return simCalls * T_CALL + rdSec * T_SEC + (uint64_t)nWrites * T_WR; }
static bool fakeRoom(uint16_t us) { return simNow() - frameT + us <= SCAN_BUDGET; }

static void testRoom(const char *label) {
    sdio::Scan s;
    sdio::ScanItem it;
    check(sdio::scanBegin(s) == sdio::OK && sdio::listBegin("GAMELIST.TXT") == sdio::OK, "scanBegin and listBegin", label);
    sdio::scanRoom = fakeRoom;
    std::vector<std::string> got;
    uint32_t frames = 0, waits = 0, lines = 0;
    uint64_t worst = 0;
    bool wrote = false, ok = true;
    sdio::Result r = sdio::BUSY;
    for (bool more = true; more && frames < 100000; frames++) {
        frameT = simNow();
        for (;;) {
            long w0 = nWrites;
            simCalls++;
            r = sdio::scanNext(s, it);
            wrote |= nWrites != w0;
            if (r == sdio::OK) {
                if (!fakeRoom(LINE_US)) { simCalls++; frameT = simNow(); frames++; }    // the line goes to the next frame
                char c[8] = "-";
                if (it.nCodes) { memcpy(c, it.codes[0], 4); c[4] = 0; }
                std::string l = std::string(it.path) + " " + c + " " + it.title;
                got.push_back(l);
                l += "\n";
                long n0 = nWrites;
                ok &= sdio::listAdd(l.c_str(), (uint16_t)l.size()) == sdio::OK && nWrites > n0;
                lines++;
            } else if (r == sdio::WAIT) { waits++; break; }
            else if (r != sdio::BUSY) { more = false; break; }
            if (!fakeRoom(sdio::SCAN_SEC_US)) break;
        }
        uint64_t d = simNow() - frameT;
        worst = d > worst ? d : worst;
    }
    sdio::scanRoom = nullptr;
    sdio::scanEnd(s);
    sdio::listEnd();
    check(r == sdio::DONE, "scanRoom: the scan ends DONE", label);
    check(got.size() == sizeof GAMES / sizeof GAMES[0], "scanRoom: every game read", std::to_string(got.size()));
    for (const Want &w : GAMES) {
        std::string up = w.path;
        for (char &ch : up) if (ch >= 'a' && ch <= 'z') ch -= 32;
        bool found = false;
        for (auto &g : got) found |= g == up + " " + w.code + " " + w.title;
        check(found, "scanRoom: the scan names every game", up);
    }
    // 512-byte clusters (no card comes so): a file open walks a FAT sector per folder sector, past SCAN_OPEN_US.
    check(worst <= SCAN_BUDGET + (strstr(label, "512") ? 1000 : 0), "scanRoom: every modelled frame within SCAN_BUDGET",
          std::to_string(worst));
    check(waits > 0, "scanRoom: WAIT ends a frame before a step that would not fit", label);
    check(!wrote, "no scanNext call writes a sector (listAdd wrote each line)", label);
    check(ok, "listAdd writes its line at once", label);
    static std::string fed;
    fed.clear();
    check(sdio::fileFeed("GAMELIST.TXT", [](char c) { fed += c; }) == sdio::OK, "GAMELIST.TXT reads back", label);
    uint32_t nl = 0;
    for (char c : fed) nl += c == 10;
    check(nl == lines, "GAMELIST.TXT has every line", std::to_string(nl));
    printf("ok   frame model (%s, 0.19): budget %u us, worst frame %.1f ms of card time, %u frames, %u WAIT\n", label,
           SCAN_BUDGET, worst / 1000.0, frames, waits);
}

int main() {
    printf("-- mount results (FAT16)\n");
    testMount(formatFat16);
    printf("-- record files (FAT16)\n");
    testRecords();
    for (int fat = 16; fat <= 32; fat += 16) {
        printf("-- power cuts and failed writes (FAT%d)\n", fat);
        if (fat == 16) formatFat16(); else formatFat32();
        testCuts(fat == 16 ? "SAVES.BAK (FAT16)" : "SAVES.BAK (FAT32)", "SAVES.BAK", "SAVES.TMP", 2, "SAVES.BK1");
        testCuts(fat == 16 ? "SAVES.UND (FAT16)" : "SAVES.UND (FAT32)", "SAVES.UND", "SAVES.UTM", 2, nullptr);
        testCuts(fat == 16 ? "SAVES/CHAA.SAV (FAT16)" : "SAVES/CHAA.SAV (FAT32)", "SAVES/CHAA.SAV", "SAVES/CHAA.TMP", 1, "SAVES/CHAA.SV1");
        testAppendCut();
        testText();
    }
    printf("-- the GAMES scan (FAT32, 512-byte clusters: every folder sector needs a FAT read)\n");
    testScan(formatFat32, "FAT32 512 B clusters");
    testRoom("FAT32 512 B clusters");
    printf("-- the GAMES scan (FAT16, 32 KB clusters, as a card formatter makes them)\n");
    testScan(formatFat16Big, "FAT16 32 KB clusters");
    testRoom("FAT16 32 KB clusters");
    printf("%d checks: %s\n", oks + fails, fails ? "FAILED" : "ALL OK");
    if (fails) printf("%d FAILED\n", fails);
    return fails ? 1 : 0;
}
