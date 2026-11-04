// SAVES.BAK on the board's card: FatFs over the CHSDtoSerial app's SD driver (disk.cpp, Sd2Card).
// GPL-3.0-or-later, as the driver is.
//
// Card access only between gfx_wait() and the next flush (the driver hands the bus back and forth itself).
#ifndef CHSIM
#include "../../sdio.h"
#include "../../chgscan.h"
#include <string.h>
extern "C" {
#include "../fatfs/ff.h"
}

namespace sdio {

static uint32_t crcRun(uint32_t c, const uint8_t *p, uint32_t n) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}
uint32_t crc32(const uint8_t *p, uint32_t n) { return ~crcRun(0xFFFFFFFFu, p, n); }

static const char TAG[8] = { 'C', 'H', 'S', 'V', 'B', 'K', '0', '1' };
static const uint16_t HEAD = 16;

extern "C" uint8_t chsd_init_err;
extern "C" uint8_t chsd_stream;         // disk.cpp: keep a CMD18 read open across sequential disk_reads
extern "C" void chsd_end(void);         // close it (before the panel uses SPI1 again)       // disk.cpp: why disk_initialize failed (1 = no answer to CMD0)
static FATFS fs;

// A card that answers but has no FAT volume (exFAT: FF_FS_EXFAT is 0) is NOT_FAT, not NO_CARD.
static Result mount() {
    FRESULT r = f_mount(&fs, "", 1);
    if (r == FR_OK) return OK;
    if (r == FR_NO_FILESYSTEM) return NOT_FAT;
    if (r == FR_NOT_READY) return chsd_init_err == 1 ? NO_CARD : IO;    // 1: CMD0 got no answer; else a card that failed
    return IO;
}

static bool writeAll(FIL *f, const void *p, UINT n) {
    UINT w = 0;
    return f_write(f, p, n, &w) == FR_OK && w == n;
}

// A write cut off between the unlink and the rename leaves only the .TMP: make it the real file again before a
// new write would truncate it.
static void promote(const char *tmp, const char *fin) {
    FILINFO fi;
    if (f_stat(fin, &fi) != FR_OK && f_stat(tmp, &fi) == FR_OK) f_rename(tmp, fin);
}

// A record file that reads back whole: tag, page count and CRC (16 bytes at a time: no page buffer).
static bool recOk(const char *name, uint8_t n) {
    FIL f;
    if (f_open(&f, name, FA_READ) != FR_OK) return false;
    uint8_t h[HEAD], b[16];
    UINT r = 0;
    bool ok = f_read(&f, h, HEAD, &r) == FR_OK && r == HEAD && !memcmp(h, TAG, 8) && h[12] == n && !h[13];
    uint32_t c = 0xFFFFFFFFu, want;
    for (UINT i = 0; ok && i < (UINT)PAGE * n; i += 16) {
        ok = f_read(&f, b, 16, &r) == FR_OK && r == 16;
        c = crcRun(c, b, 16);
    }
    f_close(&f);
    memcpy(&want, h + 8, 4);
    return ok && ~c == want;
}

// Write fin via tmp: write, read back and compare, then (bk1) fin becomes bk1, then tmp becomes fin.
static Result writeVia(const char *fin, const char *tmp, const uint8_t *a, UINT na, const uint8_t *b, UINT nb,
                       const char *bk1) {
    promote(tmp, fin);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return IO;
    bool ok = writeAll(&f, a, na) && (!nb || writeAll(&f, b, nb));
    ok = (f_close(&f) == FR_OK) && ok;
    if (!ok) return IO;
    // Read it back before it replaces the old file, 16 bytes at a time: no page buffer needed.
    UINT r = 0;
    if (f_open(&f, tmp, FA_READ) != FR_OK) return IO;
    ok = f_size(&f) == na + nb;
    for (UINT i = 0; ok && i < na + nb; i += 16) {
        uint8_t c[16];
        UINT k = na + nb - i < 16 ? na + nb - i : 16;
        ok = f_read(&f, c, k, &r) == FR_OK && r == k;
        for (UINT j = 0; ok && j < k; j++) ok = c[j] == (i + j < na ? a[i + j] : b[i + j - na]);
    }
    f_close(&f);
    if (!ok) return IO;
    if (bk1 && recOk(fin, a[12])) { f_unlink(bk1); f_rename(fin, bk1); }   // a bad old file does not replace a good bk1
    else f_unlink(fin);
    return f_rename(tmp, fin) == FR_OK ? OK : IO;
}

Result writeRec(const char *fin, const char *tmp, const uint8_t *pages, uint8_t n, const char *bk1) {
    Result m = mount();
    if (m != OK) return m;
    const char *sl = strchr(fin, '/');
    if (sl) {                                           // SAVES/...: make the folder
        char d[16];
        size_t l = (size_t)(sl - fin) < sizeof d - 1 ? (size_t)(sl - fin) : sizeof d - 1;
        memcpy(d, fin, l); d[l] = 0;
        FRESULT r = f_mkdir(d);
        if (r != FR_OK && r != FR_EXIST) return IO;
    }
    uint8_t head[HEAD];
    memcpy(head, TAG, 8);
    uint32_t crc = crc32(pages, PAGE * n);
    memcpy(head + 8, &crc, 4);
    head[12] = n; head[13] = 0;                         // pages
    head[14] = 1; head[15] = 0;                         // sets
    return writeVia(fin, tmp, head, HEAD, pages, PAGE * n, bk1);
}

static Result readOne(const char *name, uint8_t *pages, uint8_t n) {
    FIL f;
    FRESULT fr = f_open(&f, name, FA_READ);
    if (fr != FR_OK) return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? NO_FILE : IO;
    uint8_t head[HEAD];
    UINT r = 0;
    Result res = BAD_FILE;                  // read fine but not a backup (short, wrong tag, wrong CRC)
    if (f_read(&f, head, HEAD, &r) != FR_OK) res = IO;
    else if (r == HEAD && !memcmp(head, TAG, 8) && head[12] == n && head[13] == 0) {
        if (f_read(&f, pages, PAGE * n, &r) != FR_OK) res = IO;
        else if (r == (UINT)(PAGE * n)) {
            uint32_t crc;
            memcpy(&crc, head + 8, 4);
            if (crc == crc32(pages, PAGE * n)) res = OK;
        }
    }
    f_close(&f);
    return res;
}

// fin, or tmp if a write was cut off between the unlink and the rename.
Result readRec(const char *fin, const char *tmp, uint8_t *pages, uint8_t n) {
    Result m = mount();
    if (m != OK) return m;
    Result r = readOne(fin, pages, n);
    if (r == NO_FILE) r = readOne(tmp, pages, n);
    return r;
}

static bool isExt(const char *n, const char *e) {         // e: the extension in lower case
    size_t l = strlen(n);
    return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == e[0] && (n[l - 2] | 32) == e[1] && (n[l - 1] | 32) == e[2];
}

Result srcScan(uint8_t *pages, void (*cb)(const char *name, uint8_t n, Result r, const uint8_t *pages)) {
    Result m = mount();
    if (m != OK) return m;
    static const char *const fixed[3][2] = { { "SAVES.BAK", "SAVES.TMP" }, { "SAVES.BK1", nullptr }, { "SAVES.UND", "SAVES.UTM" } };
    for (int i = 0; i < 3; i++) {
        Result r = readOne(fixed[i][0], pages, 2);
        if (r == NO_FILE && fixed[i][1]) r = readOne(fixed[i][1], pages, 2);
        if (!i || r != NO_FILE) cb(fixed[i][0], 2, r, pages);
    }
    DIR d;
    if (f_opendir(&d, "SAVES") != FR_OK) return OK;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        bool tmp = isExt(fi.fname, "tmp");
        if ((fi.fattrib & AM_DIR) || (!tmp && !isExt(fi.fname, "sav"))) continue;
        char p[20] = "SAVES/";
        strncat(p, fi.fname, 13);
        char *e = p + strlen(p) - 3;
        if (tmp) {                                      // a .TMP alone (a write cut before its rename) is the .SAV
            FILINFO g;
            memcpy(e, "SAV", 3);
            if (f_stat(p, &g) == FR_OK) continue;
            memcpy(e, "TMP", 3);
        }
        Result r = readOne(p, pages, 1);
        memcpy(e, "SAV", 3);
        cb(p + 6, 1, r, pages);
    }
    f_closedir(&d);
    return OK;
}

// ---- text files ----

static Result readOneText(const char *name, char *buf, uint16_t cap, uint16_t *len, bool *more) {
    FIL f;
    FRESULT fr = f_open(&f, name, FA_READ);
    if (fr != FR_OK) return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? NO_FILE : IO;
    Result res = IO;
    FSIZE_t size = f_size(&f);
    UINT r = 0;
    UINT want = size > cap ? cap : (UINT)size;
    if (f_read(&f, buf, want, &r) == FR_OK && r == want) {
        if (size > cap) while (r && buf[r - 1] != 10) r--;      // a file over cap: whole lines of its start
        *len = (uint16_t)r;
        *more = size > cap;
        res = OK;
    }
    f_close(&f);
    return res;
}

static Result textIn(const char *fin, const char *tmp, char *buf, uint16_t cap, uint16_t *len, bool *more) {
    *len = 0; *more = false;
    Result r = readOneText(fin, buf, cap, len, more);
    if (r == NO_FILE && tmp) r = readOneText(tmp, buf, cap, len, more);
    return r;
}

Result readText(const char *fin, const char *tmp, char *buf, uint16_t cap, uint16_t *len, bool *more) {
    *len = 0; *more = false;
    Result m = mount();
    if (m != OK) return m;
    return textIn(fin, tmp, buf, cap, len, more);
}

Result writeText(const char *fin, const char *tmp, const char *buf, uint16_t len) {
    Result m = mount();
    if (m != OK) return m;
    return writeVia(fin, tmp, (const uint8_t *)buf, len, nullptr, 0, nullptr);
}

/// Append to fin (made if absent). A leading line break keeps the new lines apart from an unfinished last one.
Result appendText(const char *fin, const char *tmp, const char *buf, uint16_t len) {
    Result m = mount();
    if (m != OK) return m;
    promote(tmp, fin);
    FIL f;
    if (f_open(&f, fin, FA_WRITE | FA_OPEN_APPEND) != FR_OK) return IO;
    bool ok = writeAll(&f, "\n", 1) && writeAll(&f, buf, len);
    ok = (f_close(&f) == FR_OK) && ok;
    return ok ? OK : IO;
}

static Result findIn(const char *name, NameFinder &nf) {
    FIL f;
    FRESULT fr = f_open(&f, name, FA_READ);
    if (fr != FR_OK) return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? NO_FILE : IO;
    Result res = OK;
    for (;;) {
        char b[64];
        UINT r = 0;
        if (f_read(&f, b, sizeof b, &r) != FR_OK) { res = IO; break; }
        for (UINT i = 0; i < r; i++) nf.feed(b[i]);
        if (r < sizeof b) break;
    }
    f_close(&f);
    return res;
}

Result namesFind(const char *codes, uint8_t stride, uint8_t n, uint32_t want, uint32_t *found) {
    *found = 0;
    if (!want) return OK;
    Result m = mount();
    if (m != OK) return m;
    NameFinder nf;
    nf.begin(codes, stride, n, want);
    Result r = findIn("SAVENAME.TXT", nf);
    if (r == NO_FILE) r = findIn("SAVENAME.TMP", nf);
    if (r == OK) *found = nf.found;
    return r;
}

// ---- GAMELIST.TXT ----

static FIL lf;
static bool lOpen = false;

Result listBegin(const char *name) {
    lOpen = f_open(&lf, name, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    return lOpen ? OK : IO;
}

Result listAdd(const char *line, uint16_t len) {
    if (!lOpen) return IO;
    if (writeAll(&lf, line, len)) return OK;
    f_close(&lf); lOpen = false;
    return IO;
}

void listEnd() {
    if (lOpen) f_close(&lf);
    lOpen = false;
}

static Result feedIn(const char *path, void (*feed)(char c));

Result listFeed(char *ign, uint16_t cap, uint16_t *ilen, bool *imore, void (*feed)(char c)) {
    Result m = mount();
    if (m != OK) return m;
    if (textIn("IGNORE.TXT", "IGNORE.TMP", ign, cap, ilen, imore) == IO) return IO;
    return feedIn("GAMELIST.TXT", feed);
}

Result fileFeed(const char *path, void (*feed)(char c)) {
    Result m = mount();
    if (m != OK) return m;
    return feedIn(path, feed);
}

static Result feedIn(const char *path, void (*feed)(char c)) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? NO_FILE : IO;
    Result res = OK;
    for (;;) {
        char b[64];
        UINT r = 0;
        if (f_read(&f, b, sizeof b, &r) != FR_OK) { res = IO; break; }
        for (UINT i = 0; i < r; i++) feed(b[i]);
        if (r < sizeof b) break;
    }
    f_close(&f);
    return res;
}

// ---- the game scan ----

#if CHGAME_DEBUG
void (*scanSkip)(const char *path, const char *why) = nullptr;
#define SKIP(p, w) do { if (scanSkip) scanSkip(p, w); } while (0)
#else
#define SKIP(p, w) do { } while (0)
#endif

static chgscan::ChgScan sc;     // the read buffer and the codes; only used inside scanNext

// Folders: GAMES (level 0), its sub-folders (1) and theirs (2, e.g. GAMES/OTHER/CARDS, as the menu shows them).
static const uint8_t DEPTH = 3;
struct Priv {
    DIR dir[DEPTH];             // dir[0..level] open while scanning
    FIL f;                      // the .CHG being read while inFile
    uint32_t left;              // payload bytes still to read
    char name[DEPTH - 1][13];   // the open sub-folders' 8.3 names
    uint8_t inFile, level;
};
static_assert(sizeof(Priv) <= sizeof(Scan::priv), "Scan::priv too small");
enum { S_IDLE, S_RUN };

static bool isChg(const char *n) {
    size_t l = strlen(n);
    if (l < 4 || n[l - 4] != '.') return false;
    return (n[l - 3] | 32) == 'c' && (n[l - 2] | 32) == 'h' && (n[l - 1] | 32) == 'g';
}

static void pathOf(char *out, const Priv *p, uint8_t level, const char *leaf) {   // GAMES[/a[/b]]/leaf
    strcpy(out, "GAMES");
    for (uint8_t i = 0; i < level; i++) { strcat(out, "/"); strcat(out, p->name[i]); }
    strcat(out, "/"); strcat(out, leaf);
}

// Open a .CHG and read its header. OK: p->f open, p->left set. BAD_FILE: skip it. IO: a read failed.
static Result openChg(Priv *p, const char *path, FSIZE_t fsize, ScanItem &item) {
    if (f_open(&p->f, path, FA_READ) != FR_OK) return IO;
    UINT r = 0;
    uint32_t len = 0;
    sc.begin();
    Result res = BAD_FILE;
    if (f_read(&p->f, sc.chunk(), chgscan::CHUNK, &r) != FR_OK) res = IO;
    else if (r == chgscan::CHUNK && chgscan::header(sc.chunk(), item.title, &len) && len > 0 &&
             len <= (uint32_t)(fsize - chgscan::CHUNK)) {
        sc.begin();
        p->left = len;
        p->inFile = 1;
        return OK;
    }
    f_close(&p->f);
    return res;
}

// Read up to SCAN_STEP bytes of the open .CHG. OK: finished (item filled). BUSY: more. IO: a read failed.
static Result readSome(Priv *p, ScanItem &item) {
    for (uint16_t done = 0; p->left && done < SCAN_STEP; ) {
        UINT n = p->left < chgscan::CHUNK ? (UINT)p->left : chgscan::CHUNK, r = 0;
        if (f_read(&p->f, sc.chunk(), n, &r) != FR_OK || r != n) { f_close(&p->f); p->inFile = 0; return IO; }
        sc.feed((uint16_t)n);
        p->left -= n;
        done += (uint16_t)n;
    }
    if (p->left) return BUSY;
    f_close(&p->f);
    p->inFile = 0;
    sc.finish();
    item.nCodes = sc.nCodes;
    memcpy(item.codes, sc.codes, sizeof(item.codes));
    memcpy(item.score, sc.score, sizeof(item.score));
    return OK;
}

Result scanBegin(Scan &s) {
    s.fileIndex = 0;
    s.skipDirs = s.skipFiles = s.badFiles = 0;
    s.state = S_IDLE;
    Priv *p = (Priv *)s.priv;
    p->inFile = 0;
    p->level = 0;
    Result m = mount();
    if (m != OK) return m;
    FRESULT r = f_opendir(&p->dir[0], "GAMES");
    if (r == FR_NO_PATH || r == FR_NO_FILE) return NO_GAMES;
    if (r != FR_OK) return IO;
    s.state = S_RUN;
    return OK;
}

void scanEnd(Scan &s) {
    Priv *p = (Priv *)s.priv;
    if (p->inFile) { f_close(&p->f); p->inFile = 0; }
    if (s.state != S_IDLE) for (int i = p->level; i >= 0; i--) f_closedir(&p->dir[i]);
    p->level = 0;
    s.state = S_IDLE;
}

static Result scanOne(Scan &s, ScanItem &item) {
    Priv *p = (Priv *)s.priv;
    if (p->inFile) {
        Result r = readSome(p, item);
        if (r == IO) { scanEnd(s); return IO; }
        return r;
    }
    if (s.state != S_RUN) return DONE;
    char path[6 + 13 * DEPTH + 1];              // GAMES/8.3/8.3/8.3 (FF_USE_LFN 0)
    for (uint8_t seen = 0;; seen++) {
        if (seen >= 16) return BUSY;            // a long folder: the rest next call (one call stays short)
        FILINFO fi;
        if (f_readdir(&p->dir[p->level], &fi) != FR_OK) break;
        if (!fi.fname[0]) {                     // this folder is done
            f_closedir(&p->dir[p->level]);
            if (!p->level) { s.state = S_IDLE; return DONE; }
            p->level--;
            continue;
        }
        pathOf(path, p, p->level, fi.fname);
        if (fi.fattrib & AM_DIR) {
            if (fi.fname[0] == '.') continue;
            if (p->level + 1 >= DEPTH) {                // too deep: count it and the .CHG files right in it
                SKIP(path, "folder 3 deep");
                DIR d;
                if (f_opendir(&d, path) == FR_OK) {
                    FILINFO g;
                    while (f_readdir(&d, &g) == FR_OK && g.fname[0])
                        if (!(g.fattrib & AM_DIR) && isChg(g.fname)) s.skipFiles++;
                    f_closedir(&d);
                }
                s.skipDirs++;
                return BUSY;
            }
            if (f_opendir(&p->dir[p->level + 1], path) == FR_OK) { strcpy(p->name[p->level], fi.fname); p->level++; }
            return BUSY;                                // a new folder: its entries from the next call
        }
        if (!isChg(fi.fname)) { SKIP(path, "not .CHG"); continue; }
        // No name test for macOS "._X.CHG" junk: with FF_USE_LFN 0 it lists as "_X~1.CHG". The header check
        // drops it (and any other file that is not CHG1); only a valid header counts in fileIndex.
        if (fi.fsize < chgscan::CHUNK) { SKIP(path, "tiny"); continue; }
        strncpy(item.file, fi.fname, sizeof item.file - 1);
        item.file[sizeof item.file - 1] = 0;
        strncpy(item.path, path + 6, sizeof item.path - 1);           // without "GAMES/"
        item.path[sizeof item.path - 1] = 0;
        SKIP(path, "read");
        Result r = openChg(p, path, fi.fsize, item);
        if (r == OK) { s.fileIndex++; return BUSY; }    // the payload is read by the next calls
        if (r == IO) { SKIP(path, "open/read failed"); break; }
        SKIP(path, "bad CHG1 header or length");        // BAD_FILE: not a CHG1 file, or its length is wrong
        if (fi.fname[0] != '_') s.badFiles++;           // "._X.CHG" lists as "_X~1.CHG" (FF_USE_LFN 0)
        return BUSY;                                    // an open costs a few sectors: one a call
    }
    scanEnd(s);
    return IO;
}

// One step of the scan, with the card's read kept streaming between its sectors and closed at the end.
Result scanNext(Scan &s, ScanItem &item) {
    chsd_stream = 1;
    Result r = scanOne(s, item);
    chsd_stream = 0;
    chsd_end();
    return r;
}

}  // namespace sdio
#endif
