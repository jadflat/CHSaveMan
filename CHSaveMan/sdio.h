// The save backup files on the SD card: SAVES.BAK (and the one before it, SAVES.BK1), the undo file SAVES.UND
// (the pages as they were before the last restore or erase), and SAVES/<CODE>.SAV (one game's record).
//
//   "CHSVBK01"  u8[8]    tag and version
//   u32 crc     CRC-32 of the pages
//   u16 pages   how many 256-byte pages follow (2: 0xF500, 0xF600; 1 in a .SAV: the game's page as it was)
//   u16 sets    how many sets of pages (1 for now: the hook for several backups in one file)
//   u8 pages[256 * pages]
// All little-endian, 16 + 256 * pages bytes (SAVES.BAK 528, a .SAV 272). A .SAV is named after the code when it
// is 4 of A-Z 0-9 _ - (SAVES/CHIX.SAV), else after the code's 4 bytes in hex (SAVES/43684978.SAV).
//
// Two implementations: sd/sdio_dev.cpp (FatFs over the CHSDtoSerial app's card driver, for the board) and
// sd/sdio_sim.cpp (host files, for the simulator; it follows the board's steps: .TMP, read back, rename, and a
// remount ends a running scan).
#pragma once
#include <stdint.h>
#include <string.h>

namespace sdio {

static const uint16_t PAGE = 256;
static const uint8_t PAGES = 2;
static const uint16_t BYTES = PAGE * PAGES;     // what a backup holds

enum Result : uint8_t {
    OK,
    NO_CARD,      // no card (it does not answer)
    NOT_FAT,      // a card answers but has no FAT12/16/32 volume (exFAT, NTFS, unformatted)
    NO_FILE,      // restore: SAVES.BAK is not there
    BAD_FILE,     // restore: not a backup, or its CRC does not match
    IO,           // a read or write on the card failed (or did not verify)
    DONE,         // scanNext: no more files
    BUSY,         // scanNext: part of a .CHG read, call again (one call reads at most SCAN_STEP bytes)
    NO_GAMES      // scanBegin: the card has no GAMES folder
};

uint32_t crc32(const uint8_t *p, uint32_t n);

// Record files: the header above with n pages (SAVES.BAK and SAVES.UND: 2; SAVES/<CODE>.SAV: 1, one game's record).
/// Write `fin` from n pages: via `tmp`, read back, then (bk1 not null) the old `fin` becomes bk1, then the rename.
/// A folder in fin ("SAVES/...") is made if needed.
Result writeRec(const char *fin, const char *tmp, const uint8_t *pages, uint8_t n, const char *bk1);
/// Read `fin` (or `tmp`, a write cut off before its rename) into n pages, checking the tag, page count and CRC.
Result readRec(const char *fin, const char *tmp, uint8_t *pages, uint8_t n);
/// Write SAVES.BAK from `pages` (BYTES bytes); the old one becomes SAVES.BK1.
inline Result backup(const uint8_t *pages) { return writeRec("SAVES.BAK", "SAVES.TMP", pages, PAGES, "SAVES.BK1"); }
/// Read SAVES.BAK into `pages` (BYTES bytes), checking the tag and CRC.
inline Result restore(uint8_t *pages) { return readRec("SAVES.BAK", "SAVES.TMP", pages, PAGES); }
/// Every save file on the card, one mount: SAVES.BAK (always, NO_FILE if absent; its .TMP if cut off), SAVES.BK1 and
/// SAVES.UND (if there), then each SAVES/*.SAV in folder order (a .TMP with no .SAV is listed as that .SAV). Each is read into `pages` (512 B) and passed to
/// cb with its 8.3 name, page count and readRec's result. NO_CARD / NOT_FAT if the card does not mount.
Result srcScan(uint8_t *pages, void (*cb)(const char *name, uint8_t n, Result r, const uint8_t *pages));

// Text files in the card's root (SAVENAME.TXT, IGNORE.TXT).
/// Append lines to fin (made if absent); a line break goes first. A tmp left by an old write becomes fin first.
Result appendText(const char *fin, const char *tmp, const char *buf, uint16_t len);
/// Read fin (or tmp) into buf: at most cap bytes, whole lines (*more: the file is longer). NO_FILE if absent.
Result readText(const char *fin, const char *tmp, char *buf, uint16_t cap, uint16_t *len, bool *more);
/// Replace fin with buf (via tmp, read back, rename).
Result writeText(const char *fin, const char *tmp, const char *buf, uint16_t len);
inline Result namesAppend(const char *buf, uint16_t len) { return appendText("SAVENAME.TXT", "SAVENAME.TMP", buf, len); }
/// Search the whole of SAVENAME.TXT (or SAVENAME.TMP) for "CODE=name" lines. The codes
/// are n entries `stride` bytes apart starting at `codes` (4 chars each); only those with their bit set in `want`
/// are looked for. Bit i of *found: code i starts a line with a name. NO_FILE (found 0) if neither file is there.
Result namesFind(const char *codes, uint8_t stride, uint8_t n, uint32_t want, uint32_t *found);

// namesFind's line matcher (both implementations feed it the file a byte at a time). A line counts as the app's
// namesLoad reads it: 4 code chars, '=', then a name with at least one letter or digit.
struct NameFinder {
    const char *codes;
    uint8_t stride, n, k;
    uint32_t want, found;
    char pre[5];
    void begin(const char *c, uint8_t s, uint8_t nn, uint32_t w) { codes = c; stride = s; n = nn; want = w; found = 0; k = 0; }
    void feed(char c) {
        if (c == 10 || c == 13) { k = 0; return; }
        if (k < 5) { pre[k++] = c; return; }
        if (k > 5) return;                                  // this line is decided
        if (pre[4] != '=') { k = 6; return; }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return;
        for (uint8_t i = 0; i < n; i++)
            if ((want >> i & 1) && !memcmp(codes + (uint16_t)i * stride, pre, 4)) found |= 1u << i;
        k = 6;
    }
};

// The game scan: every *.CHG in GAMES, its sub-folders and theirs (GAMES/OTHER/CARDS), a few KB per scanNext() so the
// UI can draw (and B can stop it) between reads. Do not call backup/restore/names* between scanBegin and
// scanEnd (they remount the card, which closes the scan's folders; scanNext then answers IO).
static const uint8_t SCAN_CODES = 6;
#ifndef SAVM_SCAN_STEP
#define SAVM_SCAN_STEP 2048     // the host test builds other values (tools/tests/host_fatfs.cpp)
#endif
static const uint16_t SCAN_STEP = SAVM_SCAN_STEP;   // bytes of a .CHG read per scanNext call (4 KB: up to 20 ms on the board)

struct ScanItem {
    char title[33];             // trimmed, NUL-terminated (non-printables are '?')
    char file[13];              // the 8.3 file name (for titles with no usable characters)
    char codes[SCAN_CODES][4];  // save codes found (4 chars each, not NUL-terminated), best first
    uint8_t score[SCAN_CODES];  // each code's evidence: chgscan SURE, CALL or WORD
    uint8_t nCodes;
    char path[40];              // the path under GAMES ("SUB/FILE.CHG"), as GAMELIST.TXT gives it
};

struct Scan {
    uint16_t fileIndex;         // .CHG files with a valid CHG1 header so far (the "N GAMES" count)
    uint16_t skipDirs, skipFiles;   // folders deeper than GAMES/X/Y, and the .CHG files directly in them
    uint16_t badFiles;              // .CHG files with no valid CHG1 header or length (not macOS "._" junk)
    uint8_t state;
    alignas(8) uint8_t priv[sizeof(void *) == 4 ? 208 : 320];   // board: three FatFs DIRs, the open FIL, bytes left,
                                                                // two sub-folder names (a 64-bit host test: more)
};

/// OK, NO_GAMES (nothing to scan), NO_CARD, NOT_FAT, IO.
Result scanBegin(Scan &s);
/// OK: item filled from the next .CHG. BUSY: call again. DONE: no more files. NO_CARD / IO: stop.
Result scanNext(Scan &s, ScanItem &item);
/// Close whatever the scan has open (after DONE it is a no-op). Call it before any other card call.
void scanEnd(Scan &s);

// GAMELIST.TXT: the last scan's games, one line each. listBegin/listAdd/listEnd run during a scan (no remount);
// listFeed reads it (mounts: never during a scan). listFeed: OK, NO_FILE, NO_CARD, NOT_FAT, IO.
/// Open `name` (GAMELIST.TXT during a scan, GAMELIST.CSV in an export) for writing; no mount.
Result listBegin(const char *name);
Result listAdd(const char *line, uint16_t len);
void listEnd();
/// Read IGNORE.TXT into ign (readText's rules; none: *ilen 0), then feed GAMELIST.TXT (one mount for both).
Result listFeed(char *ign, uint16_t cap, uint16_t *ilen, bool *imore, void (*feed)(char c));
/// Any file on the card, a byte at a time (mounts). OK, NO_FILE, NO_CARD, NOT_FAT, IO.
Result fileFeed(const char *path, void (*feed)(char c));
#ifdef CHSIM
extern char simFail[16];      // the simulator's fault switch at run time (the debug hook Z); empty: $SAVM_FAIL
#endif
#if CHGAME_DEBUG
extern void (*scanSkip)(const char *path, const char *why);    // debug: a file or folder the scan passed over
#endif

}  // namespace sdio
