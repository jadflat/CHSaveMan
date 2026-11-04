// CHSaveMan: a save manager for CHGame.
//
// Every game keeps its save in the two flash pages at 0xF500 and 0xF600 (CHGame's save library). This app shows
// what is in them, erases a page, backs both up to SAVES.BAK on the SD card (the one before it is SAVES.BK1) and
// restores them, keeps each game's save in SAVES/<CODE>.SAV, and writes SAVES.UND before every restore or erase
// (UNDO LAST CHANGE puts it back). The card code is the CHSDtoSerial app's (FatFs over its card driver), so the
// app is GPL-3.0.
//
//   UP / DOWN   choose        A  pick        B  back        START (hold 3 s)  CHGame menu
//   GAMES LIST: A  details    SELECT  view (all / missing / saves / ignored)    START  export GAMELIST.CSV
#include <CHGame.h>
#include <stdio.h>
#include <string.h>
#ifdef CHSIM
#include <stdlib.h>
#endif
#include "sdio.h"
#include "chgscan.h"
#include <fonts/CHGfx_Tiny3x5.h>
#include <fonts/CHGfx_SansBold12.h>
#include "Names.h"      // the save codes and their games (tools/gen_names.py)

#define SAVM_VERSION "0.18"

enum Col : uint8_t { C_BG, C_PANEL, C_EDGE, C_TEXT, C_DIM, C_HI, C_GOOD, C_BAD, C_WARN };

// ---- The pages -----------------------------------------------------------------------------------------------

struct Info {
    enum Kind : uint8_t { CODE, EMPTY, DAMAGED, RECORD, OFF } kind;   // OFF: a flash write failed, saving is off
    bool valid;          // RECORD: the CRC matches at some size
    char magic[5];
    uint8_t version, flag;
    uint16_t seq;
    uint16_t bytes;      // RECORD: the data's size, rounded up to 4
    bool spare;          // an older record of a game whose newer record is in the other page
};

static uint32_t crcOf(const uint8_t *p, uint32_t n) { return sdio::crc32(p, n); }

// A record is  u32 magic; u8 version, flag; u16 seq; data; (pad to 4) u32 crc.  The size is not stored, so try each.
__attribute__((noinline)) static void inspect(const uint8_t *p, Info &in) {
    memset(&in, 0, sizeof in);
    if (!p) { in.kind = Info::CODE; return; }
    bool ff = true, zero = true;
    for (int i = 0; i < 256; i++) { ff &= p[i] == 0xFF; zero &= p[i] == 0; }
    if (ff || zero) { in.kind = Info::EMPTY; return; }
    for (int i = 0; i < 4; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) { in.kind = Info::DAMAGED; return; }
        in.magic[i] = (char)p[i];
    }
    in.kind = Info::RECORD;
    in.version = p[4]; in.flag = p[5];
    in.seq = (uint16_t)(p[6] | p[7] << 8);
    for (uint16_t s = 0; s <= 256 - 12; s += 4) {
        uint16_t at = 8 + s;
        uint32_t c = (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24;
        if (crcOf(p, at) == c) { in.valid = true; in.bytes = s; return; }
    }
}

// Saving is off after a flash write failed (the library's BROKEN, until the board restarts): rawPage() is then
// null for both pages, which must not read as "game code".
#if CHGAME_DEBUG
static bool failArm = false, failed = false;   // the debug hook F: the next write fails as a real one would
static bool verifyArm = false;                 // Y: the next write reads back wrong
static bool cutArm = false, cutNow = false;    // U: the power goes after the next page write (a restore stops)
#endif
__attribute__((noinline)) static bool flashOff() {
#if CHGAME_DEBUG
    if (failed) return true;
#endif
    return save::status() == save::BROKEN;
}
__attribute__((noinline)) static const uint8_t *pageAt(int i) { return flashOff() ? nullptr : save::rawPage((uint8_t)i); }
static char wrote[4];                   // the pages the last operation wrote, in order (the tests check it)
// Write a page and read it back: a page that does not hold what was written is a failed write.
static bool writePage(int i, const uint8_t *b) {
    size_t n = strlen(wrote);
    if (n < 3) { wrote[n] = (char)('A' + i); wrote[n + 1] = 0; }
#if CHGAME_DEBUG
    if (failArm) { failArm = false; failed = true; return false; }
#endif
    if (!save::rawWrite((uint8_t)i, b)) return false;
    const uint8_t *q = save::rawPage((uint8_t)i);
    bool ok = q && !memcmp(q, b, 256);
#if CHGAME_DEBUG
    if (verifyArm) { verifyArm = false; ok = false; }
    if (cutArm) { cutArm = false; cutNow = true; }
#endif
    return ok;
}

// ---- Names ---------------------------------------------------------------------------------------------------
// SAVENAME.TXT on the card (lines CODE=NAME) wins over the built-in table (Names.h). The file is never held in
// RAM: the names of the codes on screen are looked up in the whole file (cache[]), and a scan keeps only the codes
// it finds that neither the file nor the table knows (learned[], appended to the file when the scan ends).

static const int MAX_LEARNED = 32;      // new codes in one scan; more: NAME LIST FULL, the next scan finds the rest
static SaveName learned[MAX_LEARNED];
static int nLearned = 0;
static uint32_t fresh = 0;              // learned[i] came from a scan and is not in SAVENAME.TXT yet
static bool namesDirty = false;         // fresh != 0: write SAVENAME.TXT at the next chance
static bool namesFull = false;          // a scan found a code with no room left for it

// buf: a backup in transit (0..511: the pages to put back; 512..1023: the pages as they are, for SAVES.UND), a
// .SAV in transit (768..1023), SAVENAME.TXT's codes during a scan, the start of IGNORE.TXT while the list is read.
alignas(4) static uint8_t buf[1024];
static const uint16_t TXT_CAP = sizeof buf;
static uint16_t nFileCodes = 0;         // during a scan: SAVENAME.TXT's codes, 4 bytes each, in buf

struct Named { char code[4]; char name[15]; uint8_t st; };   // st: 0 free, 1 to look up, 2 found, 3 not in the file
static const uint8_t NCACHE = 14;
static Named cache[NCACHE];
static bool namesStale = true;          // the codes on screen changed: look their names up (outside a scan)

static int findLearned(const char *code) {
    for (int i = 0; i < nLearned; i++) if (!memcmp(learned[i].code, code, 4)) return i;
    return -1;
}
static const char *findBuiltIn(const char *code) {        // the table's name for a code, or null
    for (const char *p = SAVE_NAMES; *p; p += 5 + strlen(p + 4)) if (!memcmp(p, code, 4)) return p + 4;
    return nullptr;
}
static int findCached(const char *code) {
    for (int i = 0; i < NCACHE; i++) if (cache[i].st == 2 && !memcmp(cache[i].code, code, 4)) return i;
    return -1;
}
static bool inFile(const char *code) {
    for (uint16_t i = 0; i < nFileCodes; i++) if (!memcmp(buf + 4 * i, code, 4)) return true;
    return false;
}
__attribute__((noinline)) static bool nameKnown(const char *code) { return findCached(code) >= 0 || findLearned(code) >= 0 || findBuiltIn(code); }
static bool codeKnown(const char *code) { return nameKnown(code) || inFile(code); }     // during a scan

// The game a code belongs to (SAVENAME.TXT first, then the table), or the code itself.
__attribute__((noinline)) static const char *gameName(const char *code) {
    int i = findCached(code);
    if (i >= 0) return cache[i].name;
    i = findLearned(code);
    if (i >= 0) return learned[i].name;
    const char *t = findBuiltIn(code);
    return t ? t : code;
}

// A title cleaned to the font and 14 characters; returns its length (0: nothing usable).
static int cleanName(char out[15], const char *t, int len) {
    int k = 0;
    for (int i = 0; i < len && k < 14; i++) {
        char c = t[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '\'' || c == '&' || c == '+' || c == '-')) continue;
        if (c == ' ' && (k == 0 || out[k - 1] == ' ')) continue;
        out[k++] = c;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
    return k;
}

static bool addLearned(const char *code, const char *name) {
    if (nLearned >= MAX_LEARNED) return false;
    memcpy(learned[nLearned].code, code, 4);
    strcpy(learned[nLearned].name, name);
    nLearned++;
    return true;
}

enum Learn : uint8_t { L_NEW, L_KNOWN, L_FULL, L_NONAME };

// A code found by the scan: new only if neither SAVENAME.TXT nor the table has it. A title with no usable
// characters (all non-ASCII, say) falls back to the 8.3 file name without its extension.
static Learn learnScan(const char *code, const char *title, const char *file) {
    if (codeKnown(code)) return L_KNOWN;
    char n[15];
    if (!cleanName(n, title, (int)strlen(title))) {
        int l = 0;
        while (file[l] && file[l] != '.') l++;
        if (!cleanName(n, file, l)) return L_NONAME;
    }
    if (!addLearned(code, n)) { namesFull = true; return L_FULL; }
    fresh |= 1u << (nLearned - 1);
    namesDirty = true;
    return L_NEW;
}

// A card text file a line at a time (SAVENAME.TXT, GAMELIST.TXT); longer lines are cut.
static char lineBuf[96];
static uint8_t lineLen = 0;
static void (*onLine)(char *l, uint8_t n) = nullptr;
static void lineFeed(char c) {
    if (c == 13) return;
    if (c == 10) {
        lineBuf[lineLen] = 0;
        if (lineLen) onLine(lineBuf, lineLen);
        lineLen = 0;
        return;
    }
    if (lineLen < sizeof lineBuf - 1) lineBuf[lineLen++] = c;
}
static void lineEnd() { if (lineLen) lineFeed(10); }
static sdio::Result feedNames(void (*h)(char *, uint8_t)) {
    onLine = h; lineLen = 0;
    sdio::Result r = sdio::fileFeed("SAVENAME.TXT", lineFeed);
    lineEnd();
    if (r == sdio::NO_FILE) { lineLen = 0; r = sdio::fileFeed("SAVENAME.TMP", lineFeed); lineEnd(); }
    return r;
}
// A SAVENAME.TXT line: 4 code characters, '=', a name with something usable in it.
static bool nameLine(const char *l, uint8_t n, char out[15]) {
    if (n <= 5 || l[4] != '=') return false;
    for (int k = 0; k < 4; k++) if (l[k] <= ' ' || l[k] >= 127) return false;
    return cleanName(out, l + 5, n - 5) > 0;
}
static void lookupLine(char *l, uint8_t n) {           // the first line of a code wins
    char nm[15];
    if (!nameLine(l, n, nm)) return;
    for (int i = 0; i < NCACHE; i++)
        if (cache[i].st == 1 && !memcmp(cache[i].code, l, 4)) { strcpy(cache[i].name, nm); cache[i].st = 2; }
}
static void codeLine(char *l, uint8_t n) {             // a scan: every code in the file
    char nm[15];
    if (nameLine(l, n, nm) && nFileCodes < TXT_CAP / 4 && !inFile(l)) memcpy(buf + 4 * nFileCodes++, l, 4);
}

static const char *sdText(sdio::Result r);
static uint16_t namesOld = 0;

// Append the fresh names to SAVENAME.TXT (never rewritten, so a big or foreign file is left alone). nullptr: saved.
// A code already in the file is not appended again: the whole file is searched first. namesOld counts those (the
// scan called them NEW; they are KNOWN).
static const char *namesSave() {
    uint32_t inF = 0;
    sdio::Result fr = sdio::namesFind(learned[0].code, sizeof learned[0], (uint8_t)nLearned, fresh, &inF);
    if (fr != sdio::OK && fr != sdio::NO_FILE) return sdText(fr);
    for (int i = 0; i < nLearned; i++) if (inF & (1u << i)) namesOld++;
    fresh &= ~inF;
    uint16_t len = 0;
    for (int i = 0; i < nLearned; i++) {
        if (!(fresh & (1u << i))) continue;
        uint16_t l = (uint16_t)strlen(learned[i].name);
        if (len + 6 + l > TXT_CAP) break;                    // the rest waits for the next save
        memcpy(buf + len, learned[i].code, 4); len += 4;
        buf[len++] = '=';
        memcpy(buf + len, learned[i].name, l); len += l;
        buf[len++] = 10;
    }
    sdio::Result r = len ? sdio::namesAppend((const char *)buf, len) : sdio::OK;
    if (r != sdio::OK) return sdText(r);
    fresh = 0;
    namesDirty = false;
    memset(cache, 0, sizeof cache);                         // the file changed: look the names up again
    namesStale = true;
    return nullptr;
}

static Info info[2];
static Info bak[2];                    // the pages a restore would put back (SAVES.BAK, SAVES.UND or a .SAV)
static bool same[2];                   // that page of bak already matches the flash page
static Info onCard[2];                 // SAVES.BAK's pages as last read or written (the main screen's BAK line)
static uint8_t onCardR = 255;          // the sdio::Result of that read (255: not read yet)

// GAMES LIST details: one line of GAMELIST.TXT, and the game's .SAV.
struct Detail { char path[40]; char code[16]; char name[34]; bool ign; sdio::Result savR; Info sav; };
static Detail det;
__attribute__((noinline)) static bool codeClear(const char *c) { return strlen(c) == 4 && !strchr(c, '|'); }

// RESTORE's list of the save files on the card (read again at each scroll: only the rows on screen are kept).
struct SrcRow { char file[13]; uint8_t n, r; char m[2][5]; uint16_t seq[2]; uint8_t ver[2]; uint8_t kind[2]; bool valid[2]; };
static const uint8_t SRC_ROWS = 5;
static SrcRow srcRow[SRC_ROWS];
static uint16_t srcTop = 0, srcN = 0;
static uint8_t srcCur = 0;
static int16_t srcPick = -1;            // the entry whose file a pick wants
static uint8_t srcPickN = 0, srcPickR = 0;
static char srcFile[20] = "SAVES.BAK", srcTmpF[20] = "SAVES.TMP";
static bool srcLoad = false;

// Look up the names of the codes on screen (both pages, the backup being asked about, SAVES.BAK, the details).
static void namesRefresh() {
    const char *w[13];
    uint8_t n = 0;
    for (int i = 0; i < 2; i++) {
        if (info[i].kind == Info::RECORD) w[n++] = info[i].magic;
        if (bak[i].kind == Info::RECORD) w[n++] = bak[i].magic;
        if (onCardR == sdio::OK && onCard[i].kind == Info::RECORD) w[n++] = onCard[i].magic;
    }
    if (codeClear(det.code)) w[n++] = det.code;
    for (uint8_t i = 0; i < SRC_ROWS && srcTop + i < srcN; i++)
        for (uint8_t k = 0; k < srcRow[i].n; k++) if (srcRow[i].kind[k] == Info::RECORD && n < 13) w[n++] = srcRow[i].m[k];
    Named old[NCACHE];
    memcpy(old, cache, sizeof cache);
    memset(cache, 0, sizeof cache);
    uint8_t k = 0;
    bool need = false;
    for (uint8_t j = 0; j < n; j++) {
        bool dup = false;
        for (uint8_t i = 0; i < k; i++) dup |= !memcmp(cache[i].code, w[j], 4);
        if (dup) continue;
        cache[k] = Named{};
        memcpy(cache[k].code, w[j], 4);
        cache[k].st = 1;
        for (uint8_t i = 0; i < NCACHE; i++) if (old[i].st >= 2 && !memcmp(old[i].code, w[j], 4)) cache[k] = old[i];
        need |= cache[k].st == 1;
        k++;
    }
    if (!need) return;
    sdio::Result r = feedNames(lookupLine);
    if (r == sdio::OK || r == sdio::NO_FILE)
        for (uint8_t i = 0; i < k; i++) if (cache[i].st == 1) cache[i].st = 3;
}

// The spare: both pages are records of one game and the other is newer (the flash pages, or SAVES.BAK's).
static void markSpare(Info *in) {
    for (int i = 0; i < 2; i++) {
        const Info &a = in[i], &b = in[1 - i];
        if (a.kind == Info::RECORD && b.kind == Info::RECORD && !memcmp(a.magic, b.magic, 4) && b.valid &&
            (int16_t)(b.seq - a.seq) > 0) in[i].spare = true;
    }
}

__attribute__((noinline)) static void scan() {
    bool off = flashOff();
    for (int i = 0; i < 2; i++) {
        inspect(pageAt(i), info[i]);
        if (off) info[i].kind = Info::OFF;
    }
    markSpare(info);
    namesStale = true;
}

// A record whose CRC does not match at any size and whose code no one knows: most likely another game's own
// save format, not a damaged CHGame record.
static bool otherFormat(const Info &in) { return in.kind == Info::RECORD && !in.valid && !nameKnown(in.magic); }
static bool keeps(const Info &in) { return in.kind == Info::EMPTY || (in.kind == Info::RECORD && in.valid && in.spare); }
__attribute__((noinline)) static bool sameGame(const Info &a, const Info &b) { return a.kind == Info::RECORD && b.kind == Info::RECORD && !memcmp(a.magic, b.magic, 4); }

// Where a game's code is: bit 0 flash page A, bit 1 page B, bit 2 SAVES.BAK (either page).
__attribute__((noinline)) static uint8_t whereIs(const char *c) {
    uint8_t m = 0;
    for (int p = 0; p < 2; p++) {
        if (info[p].kind == Info::RECORD && !memcmp(info[p].magic, c, 4)) m |= 1 << p;
        if (onCardR == sdio::OK && onCard[p].kind == Info::RECORD && !memcmp(onCard[p].magic, c, 4)) m |= 4;
    }
    return m;
}

// SAVES.BAK holds this flash page exactly (the main screen's BAK / NEW tag).
static bool inBak(int i) {
    return onCardR == sdio::OK && sameGame(info[i], onCard[i]) && onCard[i].seq == info[i].seq &&
           onCard[i].version == info[i].version;
}

// ---- Screens -------------------------------------------------------------------------------------------------

enum Screen : uint8_t { MAIN, PAGE_MENU, CONFIRM, WORKING, MSG, SCANNING, LIST, CHOOSE, DETAIL, SRCLIST };
enum Op : uint8_t { OP_NONE, OP_ERASE, OP_BACKUP, OP_BACKUP2, OP_READBAK, OP_RESTORE, OP_SCAN, OP_READUND, OP_GAMEBAK,
                    OP_GAMEBAK2, OP_DETAIL, OP_IGNORE, OP_GAMERES, OP_EXPORT, OP_OFFER, OP_SRCLIST, OP_SRCPICK, OP_ERASE2 };
enum Src : uint8_t { SRC_BAK, SRC_UND, SRC_SAV };

static Screen screen = MAIN;
static uint8_t sel = 0;                // main list: 0,1 pages; 2 back up; 3 restore; 4 find names; 5 games list; 6 undo
static uint8_t page = 0;               // the page a menu is about
static uint8_t menuSel = 0;
static uint8_t which = 2;              // the pages a back up / restore is about: 0 A only, 1 B only, 2 both
static Op chooseOp = OP_NONE;          // CHOOSE: OP_BACKUP, OP_READBAK or OP_GAMERES
static uint8_t src = SRC_BAK;          // where a restore's pages come from
static bool inSet(int i) { return which == 2 || which == i; }
static Op pending = OP_NONE;
static const char *msg1 = "", *msg2 = "", *msg3 = "";
static uint8_t msgCol = C_TEXT;
static uint8_t workWait = 0;
static char m2[28], m3[36];
static bool startTap = false;
static bool cardAgain = false;         // the card was missing or failing: read SAVES.BAK again on the main screen          // START went up after less than 1 s (GAMES LIST: export)

static sdio::Scan scn;
static sdio::ScanItem item;
static uint16_t scanFiles = 0, scanNew = 0, scanKnown = 0, scanUnsure = 0;
static bool scanStop = false;
static bool listOk = false;            // GAMELIST.TXT is being written by this scan

// The GAMES LIST screen pages GAMELIST.TXT from the card (nothing of it is kept but the rows on screen).
static const uint8_t LIST_ROWS = 11;
static char listRow[LIST_ROWS][38];   // "CODE NAME" ('-' for no code, "A|B" for unsure), as read from the file
static uint16_t listTop = 0, listN = 0;           // listN: the rows of this view
static uint16_t listAll = 0, listMiss = 0, listIgn = 0;   // all games not ignored, those without a clear code, ignored
static uint8_t listCur = 0, listMode = 0;         // the row picked on screen; 0 all, 1 missing, 2 saves, 3 ignored
static bool listLoad = false;          // read the file on the next frame (between gfx_wait and the flush)
static int32_t detWant = -1;           // the row of this view whose line goes to det
static uint16_t ignLen = 0;            // IGNORE.TXT's start, in buf while the list is read
static bool ignMore = false;
#if CHGAME_DEBUG
static uint32_t scanT0 = 0, scanMs = 0, scanCalls = 0, scanMaxUs = 0;   // debug: scan timing (STATE scan=)
#endif

__attribute__((noinline)) static void message(const char *a, const char *b, uint8_t col, const char *c = "") {
    msg1 = a; msg2 = b; msg3 = c; msgCol = col; screen = MSG;
}

__attribute__((noinline)) static const char *sdText(sdio::Result r) {
    switch (r) {
        case sdio::NO_CARD: return "NO CARD";
        case sdio::NOT_FAT: return "CARD NOT FAT32";
        case sdio::NO_FILE: return "NO SAVES.BAK";
        case sdio::BAD_FILE: return "BAD BACKUP FILE";
        case sdio::NO_GAMES: return "NO GAMES FOLDER";
        default: return "CARD ERROR";
    }
}

static bool nothing(int i) { return info[i].kind == Info::EMPTY || info[i].kind == Info::CODE; }
static bool blank(const uint8_t *p) { for (int i = 0; i < 256; i++) if (p[i] != 0xFF) return false; return true; }

__attribute__((noinline)) static void startConfirm(Op op) { pending = op; menuSel = 0; screen = CONFIRM; }
__attribute__((noinline)) static void work(Op op) { pending = op; screen = WORKING; workWait = 1; }

// SAVES/<CODE>.SAV (or the code's bytes in hex when they are not all A-Z 0-9 _ -) and its temporary name.
static char savName[20], savTmp[20];
static const char *savPath(const char *code) {
    bool plain = true;
    for (int i = 0; i < 4; i++) {
        char c = code[i];
        plain &= (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    char n[9];
    if (plain) { memcpy(n, code, 4); n[4] = 0; }
    else for (int i = 0; i < 4; i++) { n[2 * i] = "0123456789ABCDEF"[(uint8_t)code[i] >> 4]; n[2 * i + 1] = "0123456789ABCDEF"[code[i] & 15]; n[8] = 0; }
    snprintf(savName, sizeof savName, "SAVES/%s.SAV", n);
    snprintf(savTmp, sizeof savTmp, "SAVES/%s.TMP", n);
    return savName;
}

static const char *srcName() { return src == SRC_SAV ? savName : srcFile; }

// The pages as they are now, to SAVES.UND (via buf's second half): what UNDO LAST CHANGE puts back.
static sdio::Result writeUndo() {
    for (int i = 0; i < 2; i++) {
        const uint8_t *p = pageAt(i);
        if (p) memcpy(buf + 512 + 256 * i, p, 256); else memset(buf + 512 + 256 * i, 0xFF, 256);
    }
    return sdio::writeRec("SAVES.UND", "SAVES.UTM", buf + 512, 2, nullptr);
}

static void readCardBak() {
    sdio::Result r = sdio::restore(buf);
    if (r == sdio::OK) for (int i = 0; i < 2; i++) inspect(buf + 256 * i, onCard[i]);
    onCardR = r;
    namesStale = true;
}

static void doRestore() {
    // Skip pages that already match; write the page holding the older (or invalid, or empty) record first,
    // so a power cut leaves the newer record intact. A page that cannot be written is reported. SAVES.UND gets
    // the pages as they are first (a restore from SAVES.UND swaps the two).
    bool any = false;
    for (int i = 0; i < 2; i++) {
        const uint8_t *cur = pageAt(i);
        any |= inSet(i) && cur && memcmp(cur, buf + 256 * i, 256);
    }
    if (any) {
        sdio::Result u = writeUndo();
        if (u != sdio::OK) { message("RESTORE FAILED", "SAVES.UND NOT WRITTEN", C_BAD, sdText(u)); return; }
    }
    auto stale = [](int i) { return info[i].kind != Info::RECORD || !info[i].valid || info[i].spare; };
    int first = (!stale(0) && stale(1)) ? 1 : 0;
    const char *why[2] = { nullptr, nullptr };
    int nWhy = 0;
    bool wroteAny = false;
    static char w[2][24];
    for (int k = 0; k < 2; k++) {
        int i = k ? 1 - first : first;
        if (!inSet(i)) continue;                                        // a one-page restore leaves the other alone
        const uint8_t *s = buf + 256 * i;
        const uint8_t *cur = pageAt(i);
        if (!cur) {
            if (blank(s) && !flashOff()) continue;                     // game code there, and nothing to put back
            snprintf(w[nWhy], sizeof w[0], flashOff() ? "PAGE %c NOT WRITTEN" : "PAGE %c IS GAME CODE", 'A' + i);
            why[nWhy] = w[nWhy]; nWhy++;
            continue;
        }
        if (!memcmp(cur, s, 256)) continue;
        if (writePage(i, s)) {
            wroteAny = true;
#if CHGAME_DEBUG
            if (cutNow) return;                                         // the power went: nothing more happens
#endif
            continue;
        }
        snprintf(w[nWhy], sizeof w[0], "PAGE %c WRITE FAILED", 'A' + i);
        why[nWhy] = w[nWhy]; nWhy++;
    }
    scan();
    if (!nWhy) {
        if (src == SRC_UND && which == 2) message("UNDONE: PAGES BACK", "FROM SAVES.UND", C_GOOD);
        else message(which == 2 ? "RESTORED FROM" : which ? "RESTORED PAGE B FROM" : "RESTORED PAGE A FROM", srcName(), C_GOOD);
    } else message(wroteAny ? "RESTORE PARTLY DONE" : "RESTORE FAILED", why[0], C_BAD,
                   why[1] ? why[1] : (flashOff() ? "SAVING IS OFF" : ""));
}

// A game's .SAV into page t (in buf + 256 t), then the restore question. If the other page holds the same game
// with a newer counter, the record's counter goes past it (and its CRC is made again): the game loads the newest.
static void gameRestoreInto(int t) {
    if (!pageAt(t)) { message("CAN'T RESTORE", t ? "PAGE B IS GAME CODE" : "PAGE A IS GAME CODE", C_WARN); return; }
    uint8_t *d = buf + 256 * t;
    memcpy(d, buf + 768, 256);
    const Info &o = info[1 - t];
    if (o.kind == Info::RECORD && !memcmp(o.magic, det.code, 4) && (int16_t)(det.sav.seq - o.seq) <= 0) {
        uint16_t q = (uint16_t)(o.seq + 1);
        d[6] = (uint8_t)q; d[7] = (uint8_t)(q >> 8);
        uint16_t at = 8 + det.sav.bytes;
        uint32_t c = crcOf(d, at);
        memcpy(d + at, &c, 4);
    }
    inspect(d, bak[t]);
    bak[1 - t] = info[1 - t];
    same[t] = !memcmp(pageAt(t), d, 256);
    same[1 - t] = true;
    which = (uint8_t)t;
    src = SRC_SAV;
    startConfirm(OP_RESTORE);
}

// Which page a .SAV goes to: one of the same game (the older one if both are), an empty page, else ask.
static void startGameRestore() {
    if (flashOff()) { message("CAN'T RESTORE", "SAVING IS OFF", C_BAD, "RESTART THE BOARD"); return; }
    uint8_t m = whereIs(det.code);
    bool s0 = m & 1, s1 = m & 2;
    int t = -1;
    if (s0 && s1) t = (info[0].spare || !info[0].valid) ? 0 : 1;
    else if (s0 || s1) { int p = s0 ? 0 : 1; t = info[1 - p].kind == Info::EMPTY ? 1 - p : p; }
    else if (info[0].kind == Info::EMPTY) t = 0;
    else if (info[1].kind == Info::EMPTY) t = 1;
    if (t < 0) { chooseOp = OP_GAMERES; menuSel = 1; screen = CHOOSE; return; }
    gameRestoreInto(t);
}

// ---- RESTORE: the save files on the card ----

static void srcFeed(const char *f, uint8_t n, sdio::Result r, const uint8_t *pg) {
    uint16_t v = srcN++;
    if (v >= srcTop && v < srcTop + SRC_ROWS) {
        SrcRow &w = srcRow[v - srcTop];
        memset(&w, 0, sizeof w);
        snprintf(w.file, sizeof w.file, "%s", f);
        w.n = n; w.r = r;
        for (uint8_t i = 0; i < n && r == sdio::OK; i++) {
            Info in;
            inspect(pg + 256 * i, in);
            w.kind[i] = in.kind; w.valid[i] = in.valid; w.seq[i] = in.seq; w.ver[i] = in.version;
            memcpy(w.m[i], in.magic, 5);
        }
    }
    if ((int16_t)v != srcPick) return;
    srcPickN = n; srcPickR = r;
    if (n == 1) {                                       // a game's .SAV: its page waits in buf + 768
        snprintf(srcFile, sizeof srcFile, "SAVES/%s", f);
        memset(&det, 0, sizeof det);
        det.savR = r;
        if (r == sdio::OK) {
            memcpy(buf + 768, pg, 256);
            inspect(buf + 768, det.sav);
            memcpy(det.code, det.sav.magic, 4);
        }
    } else {
        snprintf(srcFile, sizeof srcFile, "%s", f);
        snprintf(srcTmpF, sizeof srcTmpF, "%s", !strcmp(f, "SAVES.BAK") ? "SAVES.TMP" : !strcmp(f, "SAVES.UND") ? "SAVES.UTM" : f);
    }
}

static sdio::Result srcRead() {
    srcN = 0;
    for (uint8_t i = 0; i < SRC_ROWS; i++) srcRow[i].file[0] = 0;
    sdio::Result r = sdio::srcScan(buf, srcFeed);
    srcPick = -1;
    return r;
}

static void srcStep() {
    srcLoad = false;
    sdio::Result r = srcRead();
    if (r != sdio::OK) { message("RESTORE FAILED", sdText(r), C_BAD); return; }
    if (srcTop && srcTop >= srcN) { srcTop = srcN > SRC_ROWS ? srcN - SRC_ROWS : 0; srcLoad = true; }
    else if (srcTop + srcCur >= srcN) srcCur = srcN > srcTop ? (uint8_t)(srcN - srcTop - 1) : 0;
    namesStale = true;
}

// ---- GAMES LIST: GAMELIST.TXT, read from the card again at each scroll ----

static const char *leafOf(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static bool sameText(const char *a, uint16_t na, const char *b) {     // a (na chars) equals b, any case
    for (uint16_t i = 0; i < na; i++) {
        char x = a[i], y = b[i];
        if (!y) return false;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return false;
    }
    return !b[na];
}
// An IGNORE.TXT line [a, z) (spaces trimmed) names this game: its path under GAMES or its file name.
static bool ignLine(const char *t, uint16_t a, uint16_t z, const char *path) {
    while (a < z && t[a] == ' ') a++;
    while (z > a && t[z - 1] == ' ') z--;
    return z > a && t[a] != '#' && (sameText(t + a, z - a, path) || sameText(t + a, z - a, leafOf(path)));
}
static bool ignored(const char *path) {
    const char *t = (const char *)buf;
    for (uint16_t i = 0; i < ignLen; ) {
        uint16_t e = i;
        while (e < ignLen && t[e] != 10 && t[e] != 13) e++;
        if (ignLine(t, i, e, path)) return true;
        i = e + 1;
    }
    return false;
}
static bool rowMiss(const char *code) { return code[0] == '-' || strchr(code, '|'); }

static void listLine(char *l, uint8_t n) {     // PATH CODE NAME
    (void)n;
    if (l[0] == '#') return;
    char *sp = strchr(l, ' ');
    if (!sp) return;
    *sp = 0;
    char *code = sp + 1;
    const char *name = "";
    sp = strchr(code, ' ');
    if (sp) { *sp = 0; name = sp + 1; }
    bool ign = ignored(l), miss = rowMiss(code);
    if (ign) listIgn++;
    else { listAll++; if (miss) listMiss++; }
    bool show = listMode == 3 ? ign : !ign && (listMode == 0 || (listMode == 1) == miss);
    if (!show) return;
    uint16_t v = listN++;
    if (v >= listTop && v < listTop + LIST_ROWS) {
        char *r = listRow[v - listTop];
        snprintf(r, sizeof listRow[0], "%s %s", code, name);
        for (; *r; r++) if (*r >= 'a' && *r <= 'z') *r -= 32;      // the font has capitals only
    }
    if ((int32_t)v == detWant) {
        snprintf(det.path, sizeof det.path, "%s", l);
        snprintf(det.code, sizeof det.code, "%s", code);
        snprintf(det.name, sizeof det.name, "%s", name);
        for (char *c = det.name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        det.ign = ign;
    }
}

static sdio::Result listRead() {
    listN = listAll = listMiss = listIgn = 0;
    for (uint8_t i = 0; i < LIST_ROWS; i++) listRow[i][0] = 0;
    onLine = listLine; lineLen = 0;
    sdio::Result r = sdio::listFeed((char *)buf, TXT_CAP, &ignLen, &ignMore, lineFeed);
    lineEnd();
    ignLen = 0;
    return r;
}

static void listStep() {
    listLoad = false;
    sdio::Result r = listRead();
    if (r == sdio::NO_FILE) message("NO GAMELIST.TXT", "USE FIND GAME NAMES", C_WARN, "TO MAKE IT");
    else if (r != sdio::OK) message("CAN'T READ LIST", sdText(r), C_BAD);
    else if (listTop && listTop >= listN) { listTop = listN > LIST_ROWS ? listN - LIST_ROWS : 0; listLoad = true; }
    else if (listTop + listCur >= listN) listCur = listN > listTop ? (uint8_t)(listN - listTop - 1) : 0;
}

// GAMELIST.CSV: file, folder, code, title, why no save is shown (from GAMELIST.TXT and IGNORE.TXT).
static bool csvOpen = false;
static uint16_t csvRows = 0;
static void csvField(char *o, int &k, int cap, const char *s, bool quote) {
    if (quote) o[k++] = '"';
    for (; *s && k < cap - 4; s++) { if (*s == '"') o[k++] = '"'; o[k++] = *s; }
    if (quote) o[k++] = '"';
    o[k++] = ',';
}
static void csvLine(char *l, uint8_t n) {
    (void)n;
    if (l[0] == '#') return;
    if (!csvOpen) {
        csvOpen = sdio::listBegin("GAMELIST.CSV") == sdio::OK;
        static const char head[] = "file,folder,code,title,reason missing\r\n";
        if (csvOpen) sdio::listAdd(head, sizeof head - 1);
    }
    if (!csvOpen) return;
    char *sp = strchr(l, ' ');
    if (!sp) return;
    *sp = 0;
    char *code = sp + 1;
    const char *name = "";
    sp = strchr(code, ' ');
    if (sp) { *sp = 0; name = sp + 1; }
    bool ign = ignored(l);
    char folder[44] = "GAMES";
    const char *leaf = leafOf(l);
    if (leaf != l) { strcat(folder, "/"); strncat(folder, l, (size_t)(leaf - l - 1)); }
    char o[160];
    int k = 0;
    csvField(o, k, sizeof o, leaf, false);
    csvField(o, k, sizeof o, folder, true);
    csvField(o, k, sizeof o, code[0] == '-' ? "" : code, true);
    csvField(o, k, sizeof o, name, true);
    const char *why = code[0] == '-' ? "no save code found" : strchr(code, '|') ? "unsure: several codes" : "";
    if (ign) why = *why ? (code[0] == '-' ? "ignored; no save code found" : "ignored; unsure: several codes") : "ignored";
    k += snprintf(o + k, sizeof o - k, "%s\r\n", why);
    if (sdio::listAdd(o, (uint16_t)k) != sdio::OK) csvOpen = false;
    else csvRows++;
}

// ---- The other operations ----

static char rowsMsg[24];
static void doOp() {
    Op op = pending;
    pending = OP_NONE;
    wrote[0] = 0;
    if (op == OP_ERASE || op == OP_ERASE2) {
        // No undo file and the page holds a save: ask again (ERASE ANYWAY?, NO by default) before erasing.
        static sdio::Result u = sdio::OK;
        if (op == OP_ERASE) u = writeUndo();
        const Info &in = info[page];
        if (op == OP_ERASE && u != sdio::OK && in.kind == Info::RECORD && !keeps(in)) { startConfirm(OP_ERASE2); m3[0] = 0; strcpy(m2, sdText(u)); return; }
        memset(buf, 0xFF, 256);
        bool ok = writePage(page, buf);
        scan();
        if (!ok) message("ERASE FAILED", flashOff() ? "SAVING IS OFF" : "PAGE DID NOT VERIFY", C_BAD);
        else if (u == sdio::OK) message("PAGE ERASED", "UNDO: SAVES.UND", C_GOOD);
        else message("PAGE ERASED", "NO UNDO FILE", C_WARN, sdText(u));
    } else if (op == OP_BACKUP) {
        // An existing SAVES.BAK (or a cut-off SAVES.TMP) is replaced (the old one becomes SAVES.BK1): ask first.
        sdio::Result pr = sdio::restore(buf);
        if (pr != sdio::NO_FILE) {
            if (pr != sdio::OK && pr != sdio::BAD_FILE) { message("BACKUP FAILED", sdText(pr), C_BAD); return; }
            for (int i = 0; i < 2; i++) {               // what the old backup holds (the question says)
                if (pr == sdio::OK) inspect(buf + 256 * i, bak[i]); else bak[i].kind = Info::DAMAGED;
            }
            namesStale = true;
            startConfirm(OP_BACKUP2);
            return;
        }
        pending = op = OP_BACKUP2;
    }
    if (op == OP_BACKUP2) {
        // One page: the other keeps what SAVES.BAK holds for it (read again here), or is blank without a good backup.
        bool old = which != 2 && sdio::restore(buf) == sdio::OK;
        for (int i = 0; i < 2; i++) {
            const uint8_t *p = pageAt(i);
            if (!inSet(i)) { if (!old) memset(buf + 256 * i, 0xFF, 256); continue; }
            if (p) memcpy(buf + 256 * i, p, 256); else memset(buf + 256 * i, 0xFF, 256);
        }
        sdio::Result r = sdio::backup(buf);
        pending = OP_NONE;
        if (r == sdio::OK) {
            for (int i = 0; i < 2; i++) inspect(buf + 256 * i, onCard[i]);
            onCardR = sdio::OK;
            namesStale = true;
            message(which == 2 ? "BACKED UP TO" : which ? "PAGE B BACKED UP TO" : "PAGE A BACKED UP TO", "SAVES.BAK", C_GOOD);
        } else message("BACKUP FAILED", sdText(r), C_BAD);
    } else if (op == OP_READBAK || op == OP_READUND) {
        bool und = op == OP_READUND;
        if (und) { strcpy(srcFile, "SAVES.UND"); strcpy(srcTmpF, "SAVES.UTM"); }
        bool isBak = !strcmp(srcFile, "SAVES.BAK");
        sdio::Result r = sdio::readRec(srcFile, srcTmpF, buf, 2);
        if (isBak) {
            onCardR = r;
            if (r == sdio::OK) for (int i = 0; i < 2; i++) inspect(buf + 256 * i, onCard[i]);
        }
        if (und && r == sdio::NO_FILE) { message("NOTHING TO UNDO", "NO SAVES.UND", C_WARN); return; }
        if (r != sdio::OK) { message(und ? "UNDO FAILED" : "RESTORE FAILED", r == sdio::BAD_FILE && !isBak ? "BAD FILE" : r == sdio::NO_FILE && !isBak ? "FILE IS GONE" : sdText(r), C_BAD); return; }
        if (und) which = 2;
        src = !strcmp(srcFile, "SAVES.UND") ? SRC_UND : SRC_BAK;
        for (int i = 0; i < 2; i++) {
            inspect(buf + 256 * i, bak[i]);
            const uint8_t *cur = pageAt(i);
            same[i] = cur && !memcmp(cur, buf + 256 * i, 256);
        }
        markSpare(bak);
        namesStale = true;
        startConfirm(OP_RESTORE);
    } else if (op == OP_RESTORE) {
        doRestore();
    } else if (op == OP_GAMEBAK) {
        // SAVES/<CODE>.SAV: ask before replacing one that is there.
        sdio::Result r = sdio::readRec(savPath(info[page].magic), savTmp, buf + 768, 1);
        if (r == sdio::OK || r == sdio::BAD_FILE) {
            if (r == sdio::OK) inspect(buf + 768, det.sav); else det.sav.kind = Info::DAMAGED;
            startConfirm(OP_GAMEBAK2);
            return;
        }
        if (r != sdio::NO_FILE) { message("BACKUP FAILED", sdText(r), C_BAD); return; }
        op = OP_GAMEBAK2;
    } else if (op == OP_SRCLIST) {
        srcTop = 0; srcCur = 0;
        screen = SRCLIST;
        srcStep();
    } else if (op == OP_SRCPICK) {
        srcPick = (int16_t)(srcTop + srcCur);
        sdio::Result r = srcRead();
        screen = SRCLIST;
        if (r != sdio::OK) message("RESTORE FAILED", sdText(r), C_BAD);
        else if (srcPickR != sdio::OK) message("RESTORE FAILED", !strcmp(srcFile, "SAVES.BAK") ? sdText((sdio::Result)srcPickR) :
                                               srcPickR == sdio::BAD_FILE ? "BAD FILE" : sdText((sdio::Result)srcPickR), C_BAD, srcFile);
        else if (srcPickN == 2) { chooseOp = OP_READBAK; menuSel = 0; screen = CHOOSE; }
        else if (det.sav.kind != Info::RECORD || !det.sav.valid) message("CAN'T RESTORE", "NOT A VALID SAVE", C_WARN, srcFile);
        else { snprintf(savName, sizeof savName, "%s", srcFile); startGameRestore(); }
        namesStale = true;
    } else if (op == OP_DETAIL) {
        memset(&det, 0, sizeof det);
        sdio::Result r = listRead();
        detWant = -1;
        if (r != sdio::OK || !det.path[0]) { message("CAN'T READ LIST", sdText(r), C_BAD); return; }
        det.savR = sdio::NO_FILE;
        if (codeClear(det.code)) {
            det.savR = sdio::readRec(savPath(det.code), savTmp, buf + 768, 1);
            if (det.savR == sdio::OK) inspect(buf + 768, det.sav);
        }
        menuSel = 0;
        screen = DETAIL;
        namesStale = true;
    } else if (op == OP_IGNORE) {
        uint16_t len = 0;
        bool more = false;
        sdio::Result r = sdio::readText("IGNORE.TXT", "IGNORE.TMP", (char *)buf, TXT_CAP, &len, &more);
        if (r == sdio::OK || r == sdio::NO_FILE) {
            if (!det.ign) {
                char l[44];
                int k = snprintf(l, sizeof l, "%s\n", det.path);
                r = sdio::appendText("IGNORE.TXT", "IGNORE.TMP", l, (uint16_t)k);
            } else if (more) { message("IGNORE.TXT TOO BIG", "EDIT IT ON A PC", C_WARN); return; }
            else {                                      // keep every line that does not name this game
                char *t = (char *)buf;
                uint16_t o = 0;
                for (uint16_t i = 0; i < len; ) {
                    uint16_t e = i;
                    while (e < len && t[e] != 10) e++;
                    if (!ignLine(t, i, e, det.path)) {
                        memmove(t + o, t + i, e - i); o += e - i;
                        t[o++] = 10;
                    }
                    i = e + 1;
                }
                r = sdio::writeText("IGNORE.TXT", "IGNORE.TMP", t, o);
            }
        }
        if (r != sdio::OK) { message("CAN'T CHANGE", "IGNORE.TXT", C_BAD, sdText(r)); return; }
        screen = LIST;
        listLoad = true;
    } else if (op == OP_EXPORT) {
        csvOpen = false; csvRows = 0;
        onLine = csvLine; lineLen = 0;
        sdio::Result r = sdio::listFeed((char *)buf, TXT_CAP, &ignLen, &ignMore, lineFeed);
        lineEnd();
        ignLen = 0;
        bool ok = csvOpen;
        sdio::listEnd();
        csvOpen = false;
        if (r == sdio::NO_FILE) message("NO GAMELIST.TXT", "USE FIND GAME NAMES", C_WARN, "TO MAKE IT");
        else if (r != sdio::OK || !ok) message("EXPORT FAILED", sdText(r == sdio::OK ? sdio::IO : r), C_BAD);
        else { snprintf(rowsMsg, sizeof rowsMsg, "%u GAMES", csvRows); message("SAVED GAMELIST.CSV", rowsMsg, C_GOOD); }
    }
    if (op == OP_GAMEBAK2) {
        const uint8_t *p = pageAt(page);
        if (!p) { message("BACKUP FAILED", "SAVING IS OFF", C_BAD); return; }
        memcpy(buf, p, 256);
        savPath(info[page].magic);
        char old[20];                                   // the .SAV it replaces stays as SAVES/<CODE>.SV1
        strcpy(old, savTmp);
        memcpy(old + strlen(old) - 3, "SV1", 3);
        sdio::Result r = sdio::writeRec(savName, savTmp, buf, 1, old);
        if (r == sdio::OK) message("GAME BACKED UP TO", savName, C_GOOD);
        else message("BACKUP FAILED", sdText(r), C_BAD);
    }
}

// ---- The name scan ----

static void scanFinish(sdio::Result r) {
    sdio::scanEnd(scn);
    if (listOk && scn.skipDirs) {
        char l[48];
        int k = snprintf(l, sizeof l, "# SKIPPED %u FILES IN %u DEEP FOLDERS\n", scn.skipFiles, scn.skipDirs);
        sdio::listAdd(l, (uint16_t)k);
    }
    if (listOk && scn.badFiles) {
        char l[40];
        int k = snprintf(l, sizeof l, "# NOT CHG1: %u FILES\n", scn.badFiles);
        sdio::listAdd(l, (uint16_t)k);
    }
    if (listOk) sdio::listEnd();        // before namesSave remounts the card
    listOk = false;
    nFileCodes = 0;                     // buf is free again
#if CHGAME_DEBUG
    scanMs = millis() - scanT0;
#endif
    const char *nameErr = namesDirty ? namesSave() : nullptr;       // whatever stopped the scan
    scanNew -= namesOld; scanKnown += namesOld; namesOld = 0;
    bool failed = r != sdio::DONE;
    snprintf(m2, sizeof m2, "%u GAMES  %u NEW", scanFiles, scanNew);
    if (scn.skipFiles) snprintf(m3, sizeof m3, "%u KNOWN %u UNSURE %u SKIPPED", scanKnown, scanUnsure, scn.skipFiles);
    else snprintf(m3, sizeof m3, "%u KNOWN  %u UNSURE", scanKnown, scanUnsure);
    if (nameErr) message("NAMES NOT SAVED", nameErr, C_BAD, failed ? sdText(r) : m2);
    else if (failed) message("SCAN FAILED", sdText(r), C_BAD, scanNew ? "NEW NAMES SAVED" : "");
    else if (namesFull) message("NAME LIST FULL", m2, C_WARN, "SCAN AGAIN FOR MORE");
    else message(scanStop ? "SCAN STOPPED" : "SCAN DONE", m2, scanNew ? C_GOOD : C_WARN, m3);
    namesStale = true;
}

// *code: the game's save code when the answer is clear (else left alone).
static const char *consider(const sdio::ScanItem &it, const char **code) {   // what was done (for the debug trace)
    if (!it.nCodes) return "no save code";                    // the game does not save to flash (or can't tell)
    // Only a clear answer: one code, or a best code with SURE/CALL evidence that no other code matches.
    bool clear = it.nCodes == 1 || (it.score[0] >= chgscan::CALL && it.score[1] < it.score[0]);
    if (!clear) {
        // A tie: if exactly one candidate is a code SAVENAME.TXT or the table already knows, it is that one.
        int k = -1, n = 0;
        for (int i = 0; i < it.nCodes; i++) if (codeKnown(it.codes[i])) { k = i; n++; }
        if (n != 1) { scanUnsure++; return "unsure (equal codes)"; }
        *code = it.codes[k];
        scanKnown++;
        return "KNOWN (tie broken by a known code)";
    }
    *code = it.codes[0];
    switch (learnScan(it.codes[0], it.title, it.file)) {
        case L_NEW: scanNew++; return "NEW";
        case L_KNOWN: scanKnown++; return "KNOWN";
        case L_NONAME: scanUnsure++; return "unsure (no usable name)";
        case L_FULL: break;
    }
    return "name list full";
}

#if CHGAME_DEBUG
static bool scanTrace = false;      // 'X': one ITEM line per game the scan read
static void traceItem(const sdio::ScanItem &it, const char *what) {
    char s[120];
    int n = snprintf(s, sizeof s, "ITEM %s title=%s codes=", it.file, it.title);
    for (int i = 0; i < it.nCodes && n < 90; i++) n += snprintf(s + n, sizeof s - n, "%.4s/%u ", it.codes[i], it.score[i]);
    snprintf(s + n, sizeof s - n, "-> %s\n", what);
    dbg::print(s);
}
#endif

// GAMELIST.TXT: "PATH CODE NAME" a line (PATH under GAMES; CODE '-' for none, "ABCD|WXYZ" for an unsure game's
// candidates; the name is the title).
static void gameLine(const sdio::ScanItem &it, const char *code) {
    char c[16] = "-", n[15], l[96];
    if (code) { memcpy(c, code, 4); c[4] = 0; }
    else if (it.nCodes > 1) {
        int k = 0;
        for (int i = 0; i < it.nCodes && i < 3; i++) { if (i) c[k++] = '|'; memcpy(c + k, it.codes[i], 4); k += 4; }
        c[k] = 0;
    }
    const char *g = it.title;                                          // the game's full title
    if (!*g) {                                                         // none: the known name, else "?"
        g = code && nameKnown(c) ? gameName(c) : "";
        if (!cleanName(n, g, (int)strlen(g))) snprintf(n, sizeof n, "?");
        g = n;
    }
    char pth[sizeof it.path];                                          // a space in a name would end the field
    strcpy(pth, it.path);
    for (char *q = pth; *q; q++) if (*q == ' ') *q = '?';
    int k = snprintf(l, sizeof l, "%s %s %s\n", pth, c, g);
    if (sdio::listAdd(l, (uint16_t)k) != sdio::OK) listOk = false;
}

// Card reads a frame (called between gfx_wait and the flush) while the next call would still end within
// SCAN_BUDGET us, so the picture and B stay alive at 30 fps. One scanNext reads at most SCAN_STEP bytes. The guess
// for the next call is the slowest recent one (it decays by 1/16 a call): guessing with the last call (0.11 to
// 0.17) let a slow call start late (board: 40 ms frames with a 24 ms budget, one call up to 20 ms). The panel
// flush (11 ms, SPI1 is shared) and the drawing come on top. The simulator: one call a frame (repeatable tests).
// tools/tests/host_fatfs.cpp models this loop.
static const uint32_t SCAN_BUDGET = 12000;
static uint32_t scanGuess;
static void scanStep() {
    if (scanStop) { scanFinish(sdio::DONE); return; }
    uint32_t t0 = micros();
    for (;;) {
        uint32_t t1 = micros();
        sdio::Result r = sdio::scanNext(scn, item);
        uint32_t d = micros() - t1;
        scanGuess = d > scanGuess ? d : scanGuess - (scanGuess >> 4);
#if CHGAME_DEBUG
        scanCalls++;
        if (micros() - t0 > scanMaxUs) scanMaxUs = micros() - t0;      // the frame's card time so far
#endif
        scanFiles = scn.fileIndex;
        if (r == sdio::OK) {
            const char *code = nullptr;
            const char *what = consider(item, &code);
            if (listOk) gameLine(item, code);
#if CHGAME_DEBUG
            if (scanTrace) traceItem(item, what);
#endif
            (void)what;
        } else if (r != sdio::BUSY) { scanFinish(r); return; }
#ifdef CHSIM
        (void)t0;
        return;
#else
        if (micros() - t0 + scanGuess > SCAN_BUDGET) return;
#endif
    }
}

static void startScan() {
    // Names from the last scan still waiting to be written are written first (and kept if that fails). Then
    // SAVENAME.TXT's codes go to buf for the scan (KNOWN, not NEW).
    if (namesDirty) namesSave();
    if (!namesDirty) { nLearned = 0; fresh = 0; }
    namesOld = 0;
    namesFull = false;
    nFileCodes = 0;
    feedNames(codeLine);
    scanFiles = scanNew = scanKnown = scanUnsure = 0;
    scanStop = false;
    scanGuess = 0;
#if CHGAME_DEBUG
    scanT0 = millis(); scanMs = 0; scanCalls = 0; scanMaxUs = 0;
#endif
    sdio::Result r = sdio::scanBegin(scn);
    if (r == sdio::NO_GAMES) message("SCAN DONE", "NO GAMES FOLDER", C_WARN);
    else if (r != sdio::OK) message("SCAN FAILED", sdText(r), C_BAD);
    else { screen = SCANNING; listOk = sdio::listBegin("GAMELIST.TXT") == sdio::OK; }
    if (screen != SCANNING) nFileCodes = 0;
}

// The GAMES LIST details: what can be done with the row.
enum DetOpt : uint8_t { D_RESTORE, D_BACKUP, D_IGNORE, D_BACK };
static int detPage() {                  // the flash page holding this game's newest valid save, or -1
    for (int i = 0; i < 2; i++)
        if (codeClear(det.code) && info[i].kind == Info::RECORD && info[i].valid && !info[i].spare && !memcmp(info[i].magic, det.code, 4)) return i;
    return -1;
}
static uint8_t detOpts(uint8_t *o) {
    uint8_t n = 0;
    if (detPage() >= 0) o[n++] = D_BACKUP;
    o[n++] = D_IGNORE;
    o[n++] = D_BACK;
    return n;
}

// Boot: the pages hold a save SAVES.BAK does not have (or there is no SAVES.BAK): offer a backup.
static bool offerWanted() {
#ifdef CHSIM
    if (!getenv("SAVM_OFFER")) return false;      // the simulator's tests start on the menu unless asked
#endif
    if (onCardR != sdio::OK && onCardR != sdio::NO_FILE) return false;
    bool diff = false;
    for (int i = 0; i < 2; i++)
        if (info[i].kind == Info::RECORD && info[i].valid && !info[i].spare)
            diff |= onCardR == sdio::NO_FILE || !inBak(i);
    return diff;
}

static void logic() {
    bool up = chgame.repeat(UP_BUTTON, 9, 3), down = chgame.repeat(DOWN_BUTTON, 9, 3);
    bool a = chgame.justPressed(A_BUTTON), b = chgame.justPressed(B_BUTTON);
    switch (screen) {
        case MAIN:
            if (up && sel > 0) sel--;
            if (down && sel < 6) sel++;
            if (a) {
                if (sel < 2) {
                    page = sel; menuSel = 0;
                    Info::Kind k = info[page].kind;
                    if (k == Info::RECORD || k == Info::DAMAGED) screen = PAGE_MENU;
                    else message("NOTHING HERE", k == Info::EMPTY ? "THE PAGE IS EMPTY" : k == Info::OFF ? "SAVING IS OFF" : "GAME CODE, NOT A SAVE", C_WARN);
                } else if (sel == 2) {
                    if (flashOff()) message("CAN'T BACK UP", "SAVING IS OFF", C_BAD, "RESTART THE BOARD");
                    else if (nothing(0) && nothing(1)) message("NOTHING TO", "BACK UP", C_WARN);
                    else { chooseOp = OP_BACKUP; menuSel = 0; screen = CHOOSE; }
                } else if (sel == 3 || sel == 6) {
                    if (flashOff()) message(sel == 3 ? "CAN'T RESTORE" : "CAN'T UNDO", "SAVING IS OFF", C_BAD, "RESTART THE BOARD");
                    else if (sel == 6) work(OP_READUND);
                    else work(OP_SRCLIST);
                } else if (sel == 4) work(OP_SCAN);
                else { screen = LIST; listTop = 0; listCur = 0; listN = 0; listLoad = true; }
            }
            break;
        case PAGE_MENU:
            if (up && menuSel > 0) menuSel--;
            if (down && menuSel < 2) menuSel++;
            if (b) screen = MAIN;
            if (a) {
                if (menuSel == 0) startConfirm(OP_ERASE);
                else if (menuSel == 2) screen = MAIN;
                else if (info[page].kind != Info::RECORD || !info[page].valid) message("CAN'T BACK UP", "NOT A VALID SAVE", C_WARN);
                else work(OP_GAMEBAK);
            }
            break;
        case CONFIRM: {
            Screen back = (pending == OP_ERASE || pending == OP_ERASE2 || pending == OP_GAMEBAK2) ? PAGE_MENU : MAIN;
            if (chgame.justPressed(UP_BUTTON | DOWN_BUTTON)) menuSel ^= 1;
            if (b) screen = back;
            if (a) {
                if (menuSel == 1) {                                         // YES
                    if (pending == OP_OFFER) { which = 2; pending = OP_BACKUP2; }
                    screen = WORKING; workWait = 1;
                } else screen = back;
            }
            if (screen == MAIN || screen == PAGE_MENU) pending = OP_NONE;
            break;
        }
        case MSG:
            if (a || b) { screen = MAIN; cardAgain = onCardR != sdio::OK && onCardR != sdio::NO_FILE; }
            break;
        case WORKING:
            break;
        case SCANNING:
            if (b) scanStop = true;
            break;
        case CHOOSE: {
            uint8_t lo = chooseOp == OP_GAMERES ? 1 : 0;
            if (up && menuSel > lo) menuSel--;
            if (down && menuSel < 2) menuSel++;
            if (b) { screen = chooseOp == OP_GAMERES ? (src == SRC_SAV && det.path[0] ? DETAIL : MAIN) : MAIN; chooseOp = OP_NONE; }
            if (a) {
                if (chooseOp == OP_GAMERES) { chooseOp = OP_NONE; gameRestoreInto(menuSel - 1); break; }
                which = menuSel ? (uint8_t)(menuSel - 1) : 2;
                if (chooseOp == OP_BACKUP && which != 2 && nothing(which))
                    message("NOTHING TO", which ? "BACK UP IN PAGE B" : "BACK UP IN PAGE A", C_WARN);
                else work(chooseOp);
                chooseOp = OP_NONE;
            }
            break;
        }
        case LIST: {
            if (b) { screen = MAIN; break; }
            uint16_t t = listTop;
            if (up) { if (listCur) listCur--; else if (listTop) listTop--; }
            if (down && listTop + listCur + 1 < listN) { if (listCur < LIST_ROWS - 1) listCur++; else listTop++; }
            if (chgame.repeat(LEFT_BUTTON, 9, 3)) listTop = listTop > LIST_ROWS ? listTop - LIST_ROWS : 0;
            if (chgame.repeat(RIGHT_BUTTON, 9, 3) && listN > LIST_ROWS)
                listTop = listTop + 2 * LIST_ROWS <= listN ? listTop + LIST_ROWS : listN - LIST_ROWS;
            if (t != listTop) listLoad = true;
            if (chgame.justPressed(SELECT_BUTTON)) { listMode = (uint8_t)((listMode + 1) & 3); listTop = 0; listCur = 0; listLoad = true; }
            if (startTap) work(OP_EXPORT);
            else if (a && listN && listTop + listCur < listN) { detWant = listTop + listCur; work(OP_DETAIL); }
            break;
        }
        case SRCLIST: {
            if (b) { screen = MAIN; break; }
            uint16_t t = srcTop;
            if (up) { if (srcCur) srcCur--; else if (srcTop) srcTop--; }
            if (down && srcTop + srcCur + 1 < srcN) { if (srcCur < SRC_ROWS - 1) srcCur++; else srcTop++; }
            if (t != srcTop) srcLoad = true;
            else if (a && srcN) work(OP_SRCPICK);
            break;
        }
        case DETAIL: {
            uint8_t o[4], n = detOpts(o);
            if (up && menuSel > 0) menuSel--;
            if (down && menuSel + 1 < n) menuSel++;
            if (b) { screen = LIST; listLoad = true; }
            if (a) {
                uint8_t d = o[menuSel < n ? menuSel : n - 1];
                if (d == D_BACKUP) { page = (uint8_t)detPage(); work(OP_GAMEBAK); }
                else if (d == D_IGNORE) work(OP_IGNORE);
                else { screen = LIST; listLoad = true; }
            }
            break;
        }
    }
}

// ---- Drawing -------------------------------------------------------------------------------------------------

__attribute__((noinline)) static void box(int x, int y, int w, int h, uint8_t edge) {
    gfx_fillRect(x, y, w, h, edge);
    gfx_fillRect(x + 1, y + 1, w - 2, h - 2, C_PANEL);
}

// Text: Tiny3x5 (y = top of the glyphs); the title alone is SansBold12 (y = baseline).
__attribute__((noinline)) static void T(int x, int y, const char *s, uint8_t c) {
    gfx_setFont(&CHGfx_Tiny3x5);
    gfx_text(x, y + gfx_fontBaseline(), s, c);
}
__attribute__((noinline)) static void centre(int y, const char *s, uint8_t c) {
    gfx_setFont(&CHGfx_Tiny3x5);
    gfx_text((128 - gfx_textWidth(s)) / 2, y + gfx_fontBaseline(), s, c);
}
__attribute__((noinline)) static void right(int y, const char *s, uint8_t c) {
    gfx_setFont(&CHGfx_Tiny3x5);
    T(128 - 6 - gfx_textWidth(s), y, s, c);
}
__attribute__((noinline)) static void titleText(int base, const char *s, uint8_t c) {
    gfx_setFont(&CHGfx_SansBold12);
    gfx_text((128 - gfx_textWidth(s)) / 2, base, s, c);
}

__attribute__((noinline)) static void row(int y, bool hot, const char *s, uint8_t c) {
    if (hot) gfx_fillRect(4, y - 3, 120, 11, C_EDGE);
    T(8, y, s, hot ? C_TEXT : c);
}

// WCH's printf pads "%.Ns" with '0' and keeps the precision for later %s: cut by hand, print with plain %s.
__attribute__((noinline)) static const char *cutN(const char *n, uint8_t k) {
    static char b[2][41];
    static uint8_t w = 0;
    w ^= 1;
    uint8_t i = 0;
    while (n[i] && i < k && i < 40) { b[w][i] = n[i]; i++; }
    b[w][i] = 0;
    return b[w];
}
static const char *cut14(const char *n) { return cutN(n, 14); }

// A page's line on the main screen: "A  NAME" and, at the right, its counter and whether SAVES.BAK has it.
static uint8_t pageText(int i, char *s, int ns, char *t, int nt, uint8_t *tc) {
    const Info &in = info[i];
    uint8_t c = C_TEXT;
    t[0] = 0;
    *tc = C_DIM;
    if (in.kind == Info::OFF) { snprintf(s, ns, "%c  SAVING IS OFF", 'A' + i); c = C_BAD; }
    else if (in.kind == Info::CODE) { snprintf(s, ns, "%c  GAME CODE", 'A' + i); c = C_DIM; }
    else if (in.kind == Info::EMPTY) { snprintf(s, ns, "%c  EMPTY", 'A' + i); c = C_DIM; }
    else if (in.kind == Info::DAMAGED) { snprintf(s, ns, "%c  UNKNOWN DATA", 'A' + i); c = C_WARN; }
    else if (otherFormat(in)) { snprintf(s, ns, "%c  OTHER %s", 'A' + i, in.magic); c = C_WARN; }
    else {
        snprintf(s, ns, "%c  %s", 'A' + i, cutN(gameName(in.magic), 12));
        c = in.valid ? C_TEXT : C_WARN;
        if (!in.valid) { snprintf(t, nt, "BAD"); *tc = C_BAD; }
        else if (in.spare) snprintf(t, nt, "#%u OLD", in.seq);
        else if (onCardR == sdio::OK || onCardR == sdio::NO_FILE) {
            bool k = inBak(i);
            snprintf(t, nt, "#%u %s", in.seq, k ? "BAK" : "NEW");
            *tc = k ? C_GOOD : C_WARN;
        } else { snprintf(t, nt, "#%u", in.seq); *tc = C_GOOD; }
    }
    return c;
}

static void pageRow(int y, int i, bool hot) {
    char s[32], t[16];
    uint8_t tc, c = pageText(i, s, sizeof s, t, sizeof t, &tc);
    row(y, hot, s, c);
    if (t[0]) right(y, t, hot ? C_TEXT : tc);
}

// 8 bytes of a page from `at`, as hex ("FF 3A 00 ...").
static void hexOf(char *o, const uint8_t *p, int at) {
    static const char hx[] = "0123456789ABCDEF";
    int k = 0;
    for (int i = 0; p && i < 8; i++) { o[k++] = hx[p[at + i] >> 4]; o[k++] = hx[p[at + i] & 15]; o[k++] = ' '; }
    o[k] = 0;
}

// A page in a few words, cut to k characters (no %.Ns: see cut14). A record's owner is named from its code.
__attribute__((noinline)) static void what(char *out, const Info &in, int k) {
    const char *d = in.kind == Info::EMPTY ? "EMPTY" : in.kind == Info::CODE ? "GAME CODE" : in.kind == Info::OFF ? "OFF" :
                    in.kind == Info::DAMAGED ? "UNKNOWN" : otherFormat(in) ? in.magic : gameName(in.magic);
    int i = 0;
    while (d[i] && i < k) { out[i] = d[i]; i++; }
    out[i] = 0;
}

static void bakText(char *bl, int n) {
    if (onCardR == sdio::OK) {
        char x[11], y[11];
        what(x, onCard[0], 10); what(y, onCard[1], 10);
        snprintf(bl, n, "BAK  %s + %s", x, y);
    } else snprintf(bl, n, "BAK  %s", onCardR == 255 ? "?" : onCardR == sdio::NO_FILE ? "NONE" : sdText((sdio::Result)onCardR));
}

static void drawMain() {
    titleText(12, "SAVE MANAGER", C_HI);
    gfx_fillRect(4, 16, 120, 1, C_EDGE);
    T(8, 20, "FLASH SAVE PAGES", C_DIM);
    pageRow(30, 0, screen == MAIN && sel == 0);
    pageRow(41, 1, screen == MAIN && sel == 1);
    gfx_fillRect(4, 51, 120, 1, C_EDGE);
    char bl[32];
    bakText(bl, sizeof bl);
    T(8, 55, bl, C_DIM);
    static const char *const rows[5] = { "BACK UP PAGES...", "RESTORE PAGES...", "FIND GAME NAMES", "GAMES LIST", "UNDO LAST CHANGE" };
    for (int k = 0; k < 5; k++) row(65 + 9 * k, screen == MAIN && sel == k + 2, rows[k], C_TEXT);
    // What the selected line means.
    const char *h = "UP/DOWN  A: PICK";
    if (sel < 2) {
        const Info &in = info[sel];
        if (in.kind == Info::RECORD)
            h = otherFormat(in) ? "ANOTHER GAME'S FORMAT?" : !in.valid ? "DAMAGED SAVE" : in.spare ? "OLD COPY, ERASABLE" :
                inBak(sel) ? "THE SAME IN SAVES.BAK" : onCardR == sdio::OK ? "NOT IN SAVES.BAK YET" : "A GAME'S SAVE";
        else if (in.kind == Info::OFF) h = "RESTART THE BOARD";
        else if (in.kind == Info::DAMAGED) h = "A: SEE ITS FIRST BYTES";
    }
    if (sel == 4) h = "FINDS NAMES ON CARD";
    if (sel == 5) h = "GAMELIST.TXT ON CARD";
    if (sel == 6) h = "BEFORE LAST RESTORE/ERASE";
    centre(111, h, C_DIM);
    T(8, 120, "V" SAVM_VERSION "  HOLD START", C_DIM);
}

// l3 (the restore question's warning) moves the lines up to make room for a third; o2: a third option.
static void drawDialog(const char *title, const char *l1, const char *l2, const char *o0, const char *o1, uint8_t tc,
                       const char *l3 = nullptr, const char *o2 = nullptr) {
    drawMain();
    box(4, 25, 120, 68, C_EDGE);
    centre(31, title, tc);
    int y = l3 ? 40 : 43, dy = l3 ? 8 : 9;
    if (l1) centre(y, l1, C_TEXT);
    if (l2) centre(y + dy, l2, C_TEXT);
    if (l3) centre(y + 2 * dy, l3, C_WARN);
    for (int k = 0; k < 3; k++) {
        const char *o = k == 2 ? o2 : k ? o1 : o0;
        if (!o) continue;
        int oy = o2 ? 63 + 11 * k : 67 + 12 * k;
        if (menuSel == k) gfx_fillRect(8, oy - 3, 112, 11, C_EDGE);
        T(12, oy, o, C_TEXT);
    }
}

// One line a page for the restore question: what the page holds now -> what is put there (the version too when
// the same game's record version differs: the game may not load it). 28 characters at most (112 px).
static void bakLine(char *out, int n, int i) {
    char now[10], to[10];
    what(now, info[i], 9);
    what(to, bak[i], 9);
    const char *old = bak[i].kind == Info::RECORD && bak[i].valid && bak[i].spare ? " OLD" : "";
    if (same[i]) snprintf(out, n, "%c  %s  SAME", 'A' + i, now);
    else if (sameGame(info[i], bak[i]) && info[i].version != bak[i].version)
        snprintf(out, n, "%c %s V%u -> V%u%s", 'A' + i, now, info[i].version, bak[i].version, old);
    else snprintf(out, n, "%c %s -> %s%s", 'A' + i, now, to, old);
}

// The restore question's warning: a game whose newest save is in a flash page being restored over and in neither
// page after the restore (the restored pages where restored, the flash page where not). Named from the page's code.
static const char *lostLine(char *out, int n) {
    int k = 0;
    const char *g = nullptr;
    for (int i = 0; i < 2; i++) {
        const Info &in = info[i];
        if (!inSet(i) || same[i] || in.kind != Info::RECORD || in.spare) continue;
        if (!in.valid) {                                            // a damaged record or another game's format
            if (!otherFormat(in)) continue;
            k++; g = in.magic;
            continue;
        }
        bool kept = false;
        for (int j = 0; j < 2; j++) {
            const Info &r = inSet(j) ? bak[j] : info[j];
            kept |= r.kind == Info::RECORD && r.valid && !memcmp(r.magic, in.magic, 4);
        }
        if (!kept) { k++; g = gameName(in.magic); }
    }
    if (!k) {
        // A game's .SAV over that game's own newest save (the other page holds no copy): say which counter goes.
        if (src == SRC_SAV && which != 2) {
            const Info &in = info[which];
            if (in.kind == Info::RECORD && in.valid && !in.spare && !same[which] && !sameGame(info[1 - which], in) &&
                sameGame(in, bak[which])) { snprintf(out, n, "REPLACES SAVE #%u", in.seq); return out; }
        }
        return nullptr;
    }
    if (k > 1) return "LOSES 2 GAMES' SAVES";
    snprintf(out, n, "LOSES %s SAVE", cut14(g));
    return out;
}

// The restore question's three lines (the dialog and the debug state).
static const char *askTitle() {
    return src == SRC_SAV ? "RESTORE GAME SAVE?" : which != 2 ? (which ? "RESTORE PAGE B?" : "RESTORE PAGE A?") :
           src == SRC_UND ? "UNDO LAST CHANGE?" : strcmp(srcFile, "SAVES.BK1") ? "RESTORE SAVES.BAK?" : "RESTORE SAVES.BK1?";
}
static const char *askLines(char *l1, char *l2, char *l3, int n) {
    if (which == 2) { bakLine(l1, n, 0); bakLine(l2, n, 1); }
    else {
        bakLine(l1, n, which);
        if (src == SRC_SAV) snprintf(l2, n, "FROM %s", savName);
        else snprintf(l2, n, "PAGE %c LEFT AS IT IS", 'B' - which);
    }
    return lostLine(l3, 28);
}

static void draw() {
    gfx_clear(C_BG);
    char a[28], b[36];
    switch (screen) {
        case MAIN: drawMain(); break;
        case PAGE_MENU: {
            const Info &in = info[page];
            bool rec = in.kind == Info::RECORD;
            snprintf(a, sizeof a, "PAGE %c", 'A' + page);
            snprintf(b, sizeof b, "%s  V%d #%u", rec ? in.magic : "?", in.version, in.seq);
            if (!rec) {                         // unknown data: its first 16 bytes, so the user can see what it is
                char h0[28], h1[28];
                hexOf(h0, pageAt(page), 0); hexOf(h1, pageAt(page), 8);
                drawDialog(a, h0, h1, "ERASE THIS PAGE", "BACK UP THIS GAME", C_HI, nullptr, "CANCEL");
                break;
            }
            drawDialog(a, !rec ? "NOT A GAME SAVE" : otherFormat(in) ? "OTHER FORMAT" : gameName(in.magic), rec ? b : nullptr,
                       "ERASE THIS PAGE", "BACK UP THIS GAME", C_HI, nullptr, "CANCEL");
            break;
        }
        case CONFIRM:
            if (pending == OP_ERASE2) {
                snprintf(b, sizeof b, "NO UNDO: %s", m2);
                drawDialog("ERASE ANYWAY?", b, "THE SAVE CAN'T COME BACK", "NO", "YES", C_BAD);
            } else if (pending == OP_ERASE) {
                const Info &in = info[page];
                bool save = in.kind == Info::RECORD && in.valid && !in.spare;
                snprintf(a, sizeof a, "ERASE PAGE %c?", 'A' + page);
                if (save) snprintf(b, sizeof b, "LOSES %s SAVE", cut14(gameName(in.magic)));
                else if (in.kind == Info::RECORD && !keeps(in)) snprintf(b, sizeof b, "LOSES %s DATA", cut14(otherFormat(in) ? in.magic : gameName(in.magic)));
                else snprintf(b, sizeof b, "%s", keeps(in) ? "NO SAVE LOST" : "DATA WILL BE LOST");
                drawDialog(a, b, keeps(in) ? "AN OLD COPY" : onCardR == sdio::NO_CARD ? "NO CARD: NO UNDO" : "UNDO: SAVES.UND", "NO", "YES", keeps(in) ? C_HI : C_WARN);
            } else if (pending == OP_OFFER) {
                drawDialog("BACK UP NOW?", onCardR == sdio::NO_FILE ? "NO SAVES.BAK YET" : "THE PAGES DIFFER", "FROM SAVES.BAK", "NO", "YES", C_HI);
            } else if (pending == OP_GAMEBAK2) {
                const Info &in = info[page];
                if (det.sav.kind == Info::RECORD) snprintf(a, sizeof a, "SAVED: #%u V%u", det.sav.seq, det.sav.version);
                else snprintf(a, sizeof a, "SAVED: BAD FILE");
                snprintf(b, sizeof b, "BECOMES #%u V%u", in.seq, in.version);
                drawDialog("REPLACE GAME BACKUP?", a, b, "NO", "YES", C_WARN, "OLD ONE KEPT AS .SV1");
            } else if (pending == OP_BACKUP2) {
                if (which == 2) {
                    char x[11], y[11];
                    what(x, bak[0], 10); what(y, bak[1], 10);
                    snprintf(b, sizeof b, "OLD: %s + %s", x, y);
                    drawDialog("REPLACE SAVES.BAK?", bak[0].kind == Info::DAMAGED && bak[1].kind == Info::DAMAGED ? "OLD: BAD FILE" : b,
                               "OLD ONE KEPT AS .BK1", "NO", "YES", C_WARN);
                } else {
                    // One page: SAVES.BAK's copy of it is replaced, its other page is kept (blank if the file is bad).
                    char o[10], l2[32], l3[28];
                    what(o, bak[which], 9);
                    snprintf(a, sizeof a, "REPLACE BACKUP'S %c?", 'A' + which);
                    snprintf(b, sizeof b, "BACKUP %c: %s", 'A' + which, bak[which].kind == Info::DAMAGED ? "BAD FILE" : o);
                    what(o, info[which], 9);
                    snprintf(l2, sizeof l2, "BECOMES %s", o);
                    bool lose = bak[which].kind == Info::RECORD && bak[which].valid && !bak[which].spare &&
                                !(info[which].kind == Info::RECORD && !memcmp(info[which].magic, bak[which].magic, 4));
                    if (lose) snprintf(l3, sizeof l3, "%s ONLY IN .BK1", cut14(gameName(bak[which].magic)));
                    else snprintf(l3, sizeof l3, "%s", bak[1 - which].kind == Info::DAMAGED ? "OTHER PAGE BLANK" : "OTHER PAGE KEPT");
                    drawDialog(a, b, l2, "NO", "YES", C_WARN, l3);
                }
            } else {
                char l2[36], l3[28];
                const char *w = askLines(b, l2, l3, sizeof l2);
                drawDialog(askTitle(), b, l2, "NO", "YES", C_WARN, w);
            }
            break;
        case WORKING:
            drawMain();
            box(4, 40, 120, 28, C_EDGE);
            centre(51, pending == OP_RESTORE ? "RESTORING..." : pending == OP_BACKUP || pending == OP_BACKUP2 ? "BACKING UP..." :
                       pending == OP_READBAK ? "READING SAVES.BAK" : pending == OP_EXPORT ? "WRITING GAMELIST.CSV" : "WORKING...", C_TEXT);
            break;
        case SCANNING:
            drawMain();
            box(4, 40, 120, 54, C_EDGE);
            centre(46, "SCANNING CARD", C_HI);
            snprintf(a, sizeof a, "%u GAMES", scanFiles);
            centre(56, a, C_TEXT);
            snprintf(b, sizeof b, "%u NEW  %u KNOWN", scanNew, scanKnown);
            centre(65, b, C_TEXT);
            if (scanUnsure) { snprintf(a, sizeof a, "%u UNSURE", scanUnsure); centre(74, a, C_DIM); }
            centre(84, scanStop ? "STOPPING..." : "B: STOP", C_DIM);
            break;
        case CHOOSE: {
            drawMain();
            box(4, 25, 120, 68, C_EDGE);
            bool g = chooseOp == OP_GAMERES;
            centre(31, g ? "RESTORE INTO WHICH PAGE?" : chooseOp == OP_BACKUP ? "BACK UP WHICH PAGES?" : "RESTORE WHICH PAGES?", C_HI);
            for (int k = g ? 1 : 0; k < 3; k++) {
                char n[13];
                if (k) {        // a restore: what the file holds for that page (the file list's cursor row)
                    const SrcRow &w = srcRow[srcCur];
                    const char *t = chooseOp != OP_READBAK ? nullptr : w.kind[k - 1] == Info::RECORD ? gameName(w.m[k - 1]) :
                                    w.kind[k - 1] == Info::EMPTY ? "EMPTY" : "?";
                    if (t) snprintf(n, sizeof n, "%s", cutN(t, 12)); else what(n, info[k - 1], 12);
                    snprintf(b, sizeof b, g ? "PAGE %c  %s" : "ONLY %c  %s", 'A' + k - 1, n);
                }
                int y = 43 + 12 * k;
                if (menuSel == k) gfx_fillRect(8, y - 3, 112, 11, C_EDGE);
                T(12, y, k ? b : "BOTH PAGES", C_TEXT);
            }
            centre(82, g ? "ITS SAVE IS REPLACED" : chooseOp == OP_READBAK ? srcFile : "B: CANCEL", C_DIM);
            break;
        }
        case LIST: {
            static const char *const modes[4] = { "ALL", "MISSING", "SAVES", "IGNORED" };
            titleText(12, "GAMES LIST", C_HI);
            gfx_fillRect(4, 16, 120, 1, C_EDGE);
            if (listMode == 0) snprintf(b, sizeof b, "%u GAMES %u SAVE %u MISSING", listAll, (unsigned)(listAll - listMiss), listMiss);
            else snprintf(b, sizeof b, "%s ONLY: %u", modes[listMode], listN);
            centre(20, b, C_DIM);
            if (!listN) centre(60, listMode == 3 ? "NOTHING IGNORED" : "NOTHING TO SHOW", C_DIM);
            for (uint8_t i = 0; i < LIST_ROWS && listTop + i < listN; i++) {
                const char *r = listRow[i];
                const char *sp = strchr(r, ' ');
                char c[16];
                uint8_t k = 0;
                while (r[k] && r[k] != ' ' && k < 15) { c[k] = r[k]; k++; }
                c[k] = 0;
                bool miss = c[0] == '-', unsure = strchr(c, '|') != nullptr;
                int y = 29 + 8 * i;
                if (i == listCur) gfx_fillRect(4, y - 1, 120, 7, C_EDGE);
                T(6, y, miss ? "----" : unsure ? "????" : c, miss ? C_BAD : unsure ? C_WARN : C_GOOD);
                char tg[4];                                     // A / B: in that flash page now; +: in SAVES.BAK
                uint8_t nt = 0;
                if (!miss && !unsure) {
                    uint8_t m = whereIs(c);
                    if (m & 1) tg[nt++] = 'A';
                    if (m & 2) tg[nt++] = 'B';
                    if (m & 4) tg[nt++] = '+';
                }
                tg[nt] = 0;
                T(26, y, cutN(sp ? sp + 1 : "", nt ? 21 - nt : 24), miss ? C_DIM : C_TEXT);
                if (nt) right(y, tg, C_HI);
            }
            snprintf(a, sizeof a, "%u-%u", listN ? listTop + 1 : 0, listTop + LIST_ROWS < listN ? listTop + LIST_ROWS : listN);
            T(6, 121, a, C_DIM);
            right(121, "A:INFO SEL:VIEW START:CSV", C_DIM);
            break;
        }
        case SRCLIST: {
            titleText(12, "RESTORE FROM", C_HI);
            gfx_fillRect(4, 16, 120, 1, C_EDGE);
            snprintf(b, sizeof b, "%u SAVE FILES ON THE CARD", srcN);
            centre(20, b, C_DIM);
            for (uint8_t i = 0; i < SRC_ROWS && srcTop + i < srcN; i++) {
                const SrcRow &w = srcRow[i];
                int y = 29 + 18 * i;
                if (i == srcCur) gfx_fillRect(4, y - 2, 120, 16, C_EDGE);
                T(8, y, w.file, C_HI);
                const char *st = w.r == sdio::OK ? (w.n == 2 ? "PAGES A+B" : "ONE GAME") : w.r == sdio::NO_FILE ? "NONE" :
                                 w.r == sdio::BAD_FILE ? "BAD FILE" : "CARD ERROR";
                right(y, st, w.r == sdio::OK ? C_DIM : C_WARN);
                if (w.r != sdio::OK) continue;
                char l[40] = "";
                int k = 0;
                for (uint8_t p = 0; p < w.n; p++) {
                    char nm[9], sq[8] = "";
                    const char *g = w.kind[p] == Info::EMPTY ? "-" : w.kind[p] == Info::RECORD ? gameName(w.m[p]) : "?";
                    snprintf(nm, sizeof nm, "%s", cutN(g, 8));
                    if (w.kind[p] == Info::RECORD) snprintf(sq, sizeof sq, " #%u", w.seq[p]);
                    if (w.n == 1) snprintf(l, sizeof l, "%s%s V%u%s", nm, sq, w.ver[p], w.valid[p] ? "" : " BAD");
                    else k += snprintf(l + k, sizeof l - k, "%c %s%s  ", 'A' + p, nm, sq);
                }
                T(14, y + 7, cutN(l, 27), C_TEXT);
            }
            T(6, 121, "A: PICK  B: BACK", C_DIM);
            break;
        }
        case DETAIL: {
            bool clear = codeClear(det.code);
            titleText(12, clear ? det.code : "NO CLEAR CODE", C_HI);
            gfx_fillRect(4, 16, 120, 1, C_EDGE);
            T(6, 20, cutN(det.path, det.ign ? 20 : 29), C_DIM);
            if (det.ign) right(20, "IGNORED", C_WARN);
            T(6, 28, cutN(det.name, 29), C_TEXT);
            if (clear) {
                snprintf(b, sizeof b, "SHOWN AS %s", cut14(gameName(det.code)));
                T(6, 36, b, C_TEXT);
                const char *from = findCached(det.code) >= 0 ? "SAVENAME.TXT" : findLearned(det.code) >= 0 ? "THIS SCAN" :
                                   findBuiltIn(det.code) ? "BUILT-IN TABLE" : "NOWHERE (CODE)";
                snprintf(b, sizeof b, "NAME FROM %s", from);
                T(6, 44, b, C_DIM);
                T(6, 52, "SCAN: CODE FOUND", C_GOOD);
                uint8_t m = whereIs(det.code);
                bool p0 = m & 1, p1 = m & 2;
                T(6, 60, p0 && p1 ? "FLASH: PAGES A AND B" : p0 ? "FLASH: PAGE A" : p1 ? "FLASH: PAGE B" : "FLASH: NOT THERE", C_TEXT);
                T(6, 68, m & 4 ? "SAVES.BAK: HAS IT" : "SAVES.BAK: NOT IN IT", C_TEXT);
                if (det.savR == sdio::OK && det.sav.kind == Info::RECORD)
                    snprintf(b, sizeof b, "%s: #%u V%u%s", leafOf(savName), det.sav.seq, det.sav.version, det.sav.valid ? "" : " BAD");
                else snprintf(b, sizeof b, "SAVE FILE: %s", det.savR == sdio::NO_FILE ? "NONE" : det.savR == sdio::BAD_FILE ? "BAD" : sdText(det.savR));
                T(6, 76, b, C_TEXT);
            } else {
                T(6, 36, det.code[0] == '-' ? "SCAN: NO SAVE CODE FOUND" : "SCAN: UNSURE, CANDIDATES", C_WARN);
                if (det.code[0] != '-') T(6, 44, det.code, C_TEXT);
                T(6, 52, det.code[0] == '-' ? "NO FLASH SAVE, OR NOT FOUND" : "ADD CODE=NAME TO SAVENAME.TXT", C_DIM);
            }
            uint8_t o[4], n = detOpts(o);
            static const char *const names[4] = { "RESTORE ITS SAVE", "BACK UP ITS SAVE", "IGNORE THIS GAME", "BACK" };
            for (uint8_t k = 0; k < n; k++) {
                int y = 88 + 11 * k;
                if (menuSel == k) gfx_fillRect(8, y - 3, 112, 11, C_EDGE);
                T(12, y, o[k] == D_IGNORE && det.ign ? "SHOW IT AGAIN" : names[o[k]], C_TEXT);
            }
            break;
        }
        case MSG:
            drawMain();
            box(4, 40, 120, 54, C_EDGE);
            centre(47, msg1, msgCol);
            centre(57, msg2, msgCol);
            centre(66, msg3, C_TEXT);
            centre(81, "A: OK", C_DIM);
            break;
    }
}

// ---- Debug hook (the simulator's scripts: `say` ...) --------------------------------------------------------
// Writes are queued and done after gfx_wait() like every flash write; ask G for the state a frame later.

static bool booted = false;

#if CHGAME_DEBUG
alignas(4) static uint8_t qPage[2][256];
static uint8_t qMask = 0;

static void under(char *c) { for (; *c; c++) if (*c == ' ') *c = '_'; }

static void state() {
    char s[160];
    for (int i = 0; i < 2; i++) {
        const Info &in = info[i];
        char n[15], hx[28];
        hexOf(hx, pageAt(i), 0);
        for (char *c = hx; *c; c++) if (*c == ' ') *c = '.';
        if (!hx[0]) strcpy(hx, "-");
        snprintf(n, sizeof n, "%s", in.kind == Info::RECORD ? gameName(in.magic) : "-");
        under(n);
        snprintf(s, sizeof s, "STATE page=%c kind=%d magic=%s ver=%d seq=%u valid=%d spare=%d name=%s hex=%s\n", 'A' + i, in.kind,
                 in.kind == Info::RECORD ? in.magic : "-", in.version, in.seq, in.valid, in.spare, n, hx);
        dbg::print(s);
    }
    snprintf(s, sizeof s, "STATE names=%d fresh=%u dirty=%d writes=%s\n", nLearned, (unsigned)fresh, namesDirty,
             wrote[0] ? wrote : "-");
    dbg::print(s);
    snprintf(s, sizeof s, "STATE screen=%d sel=%d which=%d pending=%d msg=%s|%s|%s\n", screen, sel, which, pending, msg1, msg2, msg3);
    dbg::print(s);
    snprintf(s, sizeof s, "STATE scan files=%u new=%u known=%u unsure=%u ms=%u calls=%u framemaxus=%u\n", scanFiles, scanNew,
             scanKnown, scanUnsure, (unsigned)scanMs, (unsigned)scanCalls, (unsigned)scanMaxUs);
    dbg::print(s);
    {
        char r0[sizeof listRow[0]], rc[sizeof listRow[0]];
        strcpy(r0, listRow[0]); under(r0);
        strcpy(rc, listRow[listCur < LIST_ROWS ? listCur : 0]); under(rc);
        snprintf(s, sizeof s, "STATE list n=%u miss=%u top=%u row0=%s all=%u ign=%u cur=%u mode=%u rowc=%s\n", listN, listMiss,
                 listTop, r0[0] ? r0 : "-", listAll, listIgn, listCur, listMode, rc[0] ? rc : "-");
        dbg::print(s);
    }
    {
        char l1[36], l2[36], t[16], r0[32];
        bakText(l1, sizeof l1); under(l1);
        uint8_t tc;
        snprintf(s, sizeof s, "STATE bak=%s", l1);
        dbg::print(s);
        for (int i = 0; i < 2; i++) {
            pageText(i, r0, sizeof r0, t, sizeof t, &tc); under(r0); under(t);
            snprintf(l2, sizeof l2, " row%c=%s|%s", 'A' + i, r0, t[0] ? t : "-");
            dbg::print(l2);
        }
        dbg::print("\n");
    }
    {
        char n[16], p[40], nm[34];
        snprintf(n, sizeof n, "%s", codeClear(det.code) ? gameName(det.code) : "-"); under(n);
        snprintf(p, sizeof p, "%s", det.path[0] ? det.path : "-");
        snprintf(nm, sizeof nm, "%s", det.name[0] ? det.name : "-"); under(nm);
        snprintf(s, sizeof s, "STATE det path=%s code=%s name=%s ign=%d sav=%d savseq=%u shown=%s\n", p, det.code[0] ? det.code : "-",
                 nm, det.ign, (int)det.savR, det.sav.seq, n);
        dbg::print(s);
    }
    {
        char l[100];
        int k = snprintf(l, sizeof l, "STATE src n=%u top=%u cur=%u files=", srcN, srcTop, srcCur);
        for (uint8_t i = 0; i < SRC_ROWS && srcTop + i < srcN; i++)
            k += snprintf(l + k, sizeof l - k, "%s%s:%u", i ? "," : "", srcRow[i].file, srcRow[i].r);
        snprintf(l + k, sizeof l - k, "\n");
        dbg::print(l);
    }
    if (screen == CONFIRM && pending == OP_RESTORE) {       // the restore question's lines
        char l1[36], l2[36], l3[28];
        const char *w = askLines(l1, l2, l3, sizeof l1);
        snprintf(s, sizeof s, "STATE ask=%s|%s|%s\n", l1, l2, w ? w : "");
    } else snprintf(s, sizeof s, "STATE ask=\n");
    dbg::print(s);
}

static void traceSkip(const char *path, const char *why) {
    char s[80];
    snprintf(s, sizeof s, "CARD %s %s\n", path, why);
    dbg::print(s);
}

// D <path>  the file as hex lines (HEX <offset> <32 bytes>), e.g. D GAMES/GAMES/STARLIFT.CHG
static uint32_t dumpAt = 0;
static char dumpLine[16 + 64 + 2];
static void dumpByte(char c) {
    static const char *hx = "0123456789ABCDEF";
    uint8_t k = dumpAt & 31;
    if (!k) snprintf(dumpLine, 16, "HEX %06X ", (unsigned)dumpAt);
    dumpLine[11 + 2 * k] = hx[(uint8_t)c >> 4];
    dumpLine[12 + 2 * k] = hx[c & 15];
    dumpAt++;
    if (!(dumpAt & 31)) { dumpLine[75] = 10; dumpLine[76] = 0; dbg::print(dumpLine); }
}

// X  scan the card now, tracing each file
// W <page> <MAGIC> <version> <seq> <n>  write a fake record of n data bytes (value i) into a page
// E <page>  erase a page    C <page>  flip a data byte (bad CRC)    F  the next write fails    G  state
// Y  the next page write reads back wrong    U  the power goes after the next page write (a restore stops there)
// O  run the boot step again (read SAVES.BAK, maybe offer a backup)
// Z <mode>  the simulator's card fault from now on (sdio_sim.cpp's SAVM_FAIL modes; "Z -": none)
static bool hook(char cmd, const char *args) {
    if (cmd == 'G') { scan(); state(); return true; }
    if (cmd == 'D') {
        while (*args == ' ') args++;
        dumpAt = 0;
        sdio::Result r = sdio::fileFeed(args, dumpByte);
        if (dumpAt & 31) { dumpLine[11 + 2 * (dumpAt & 31)] = 10; dumpLine[12 + 2 * (dumpAt & 31)] = 0; dbg::print(dumpLine); }
        char s[40];
        snprintf(s, sizeof s, "DUMP r=%d bytes=%u\n", (int)r, (unsigned)dumpAt);
        dbg::print(s);
        return true;
    }
    if (cmd == 'X') {                       // the whole name scan now, with ITEM and SKIP lines (as FIND GAME NAMES)
        scanTrace = true;
        sdio::scanSkip = traceSkip;
        startScan();
        while (screen == SCANNING) scanStep();
        scanTrace = false;
        sdio::scanSkip = nullptr;
        state();
        return true;
    }
    if (cmd == 'F') { failArm = true; return true; }
    if (cmd == 'Y') { verifyArm = true; return true; }
    if (cmd == 'O') { booted = false; return true; }
    if (cmd == 'H') {                       // H <page> <hex bytes>: raw bytes at the page's start, the rest 0xFF
        uint8_t p = (uint8_t)dbg::parseNum(args, 10) & 1;
        memset(qPage[p], 0xFF, 256);
        for (int i = 0; i < 64; i++) {
            while (*args == ' ') args++;
            if (!*args) break;
            qPage[p][i] = (uint8_t)dbg::parseNum(args, 16);
        }
        qMask |= 1 << p;
        return true;
    }       // the boot step again (SAVES.BAK, the backup offer)
    if (cmd == 'U') { cutArm = true; return true; }
#ifdef CHSIM
    if (cmd == 'Z') {
        while (*args == ' ') args++;
        snprintf(sdio::simFail, sizeof sdio::simFail, "%s", *args == '-' ? "" : args);
        return true;
    }
#endif
    if (cmd == 'E' || cmd == 'C' || cmd == 'W') {
        uint8_t p = (uint8_t)dbg::parseNum(args, 10) & 1;
        uint8_t *q = qPage[p];
        if (cmd == 'E') memset(q, 0xFF, 256);
        else if (cmd == 'C') { const uint8_t *cur = save::rawPage(p); if (!cur) return true; memcpy(q, cur, 256); q[9] ^= 0x5A; }
        else {
            while (*args == ' ') args++;
            char m[4] = { args[0], args[1], args[2], args[3] };
            args += 4;
            uint8_t ver = (uint8_t)dbg::parseNum(args, 10);
            uint16_t seq = (uint16_t)dbg::parseNum(args, 10);
            uint16_t n = (uint16_t)dbg::parseNum(args, 10) & ~3u;
            if (n > 244) n = 244;
            memset(q, 0, 256);
            memcpy(q, m, 4); q[4] = ver; q[5] = 0; q[6] = (uint8_t)seq; q[7] = (uint8_t)(seq >> 8);
            for (uint16_t i = 0; i < n; i++) q[8 + i] = (uint8_t)i;
            uint32_t c = crcOf(q, 8 + n);
            memcpy(q + 8 + n, &c, 4);
        }
        qMask |= 1 << p;
        return true;
    }
    return false;
}

static void hookWrites() {
    if (!qMask) return;
    for (int p = 0; p < 2; p++) if (qMask & (1 << p)) save::rawWrite((uint8_t)p, qPage[p]);
    qMask = 0;
    scan();
}
#endif

// ---- Main ----------------------------------------------------------------------------------------------------

void setup() {
    chgame.boot();
    chgame.startExits = false;          // the app counts START itself: 3 s at 30 fps (the library counts 60 Hz frames)
#if CHGAME_DEBUG
    dbg::begin("SAVM " SAVM_VERSION);
    dbg::hook = hook;
#endif
    gfx_begin(GFX_DIV2, GFX_12BPP);
    const uint16_t pal[16] = {
        gfx_rgb(10, 14, 28),    // C_BG
        gfx_rgb(24, 30, 56),    // C_PANEL
        gfx_rgb(50, 70, 130),   // C_EDGE
        gfx_rgb(240, 240, 240), // C_TEXT
        gfx_rgb(130, 140, 170), // C_DIM
        gfx_rgb(255, 210, 90),  // C_HI
        gfx_rgb(90, 220, 120),  // C_GOOD
        gfx_rgb(255, 80, 80),   // C_BAD
        gfx_rgb(255, 160, 60),  // C_WARN
        0, 0, 0, 0, 0, 0, 0 };
    gfx_setPalette(pal, 16);
    chgame.setFrameRate(30);
    scan();
}

static uint8_t startHeld = 0;

void loop() {
    dbg::poll();
    if (!chgame.nextFrame()) return;
    chgame.pollButtons();
    startTap = false;
    if (chgame.pressed(START_BUTTON)) { if (++startHeld >= 90) chgame.exitToMenu(); }      // 3 s at 30 fps
    else { startTap = startHeld && startHeld < 30; startHeld = 0; }
    logic();
    gfx_wait();
#if CHGAME_DEBUG
    hookWrites();
#endif
    if (!booted) {                      // once: SAVES.BAK's pages for the main screen, and maybe the backup offer
        booted = true;
        readCardBak();
        if (screen == MAIN && offerWanted()) startConfirm(OP_OFFER);
    }
    if (screen == WORKING && pending != OP_NONE) {
        if (workWait) workWait--;       // the "working" picture goes out first; the card is used on the next frame
        else if (pending == OP_SCAN) { pending = OP_NONE; startScan(); }
        else doOp();
    } else if (screen == SCANNING) scanStep();
    else if (screen == LIST && listLoad) listStep();
    else if (screen == SRCLIST && srcLoad) srcStep();
    if (cardAgain && screen == MAIN) { cardAgain = false; readCardBak(); }
    if (namesStale && screen != SCANNING) { namesStale = false; namesRefresh(); }
    draw();
    gfx_flushAsync();
}
