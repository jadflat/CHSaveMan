// The SD card in the simulator: host files that follow the board's steps (sdio_dev.cpp).
//   SAVES.BAK     $SAVM_BAK (default savm_sim.bak, removed at start so runs repeat); temporary <that>.tmp,
//                 SAVES.BK1 / .UND / .UTM are <that>.bk1 / .und / .utm; SAVES/<CODE>.SAV is under $SAVM_CARD
//   SAVENAME.TXT  and GAMES/**.CHG in $SAVM_CARD (default savm_card)
//   $SAVM_NOCARD  1: an empty slot (NO_CARD); fat: a card that answers with no FAT volume (NOT_FAT);
//                 err: a card that fails its init (IO: CARD ERROR). At run time "Z nocard|nofat|cardfail" does the same.
//   $SAVM_FAIL    io: every file read and write fails (IO); crc: the read-back check of a written file fails
//                 (the .TMP stays, the old file is kept); scan: the end of the GAMES listing fails (IO);
//                 append (or names): only the SAVENAME.TXT append fails (IO), so names are found and stay dirty;
//                 cut: a write stops after the old file is removed, before the rename (a power cut: .TMP alone);
//                 the debug hook "Z <mode>" sets the mode at run time ("Z -" goes back to $SAVM_FAIL);
//                 read: reading SAVES.BAK fails after it opened (f_read's error: IO, not BAD_FILE)
// Every card call "mounts" (a counter); a scan started before the latest mount answers IO, as FatFs does
// when the volume is remounted under open folders.
#ifdef CHSIM
#include "../../sdio.h"
#include "../../chgscan.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sdio {

uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static const char TAG[8] = { 'C', 'H', 'S', 'V', 'B', 'K', '0', '1' };
static const uint16_t HEAD = 16;

static const char *env(const char *n) { const char *p = getenv(n); return p ? p : ""; }
char simFail[16] = "";
static bool fault(const char *k) {
    const char *f = simFail[0] ? simFail : env("SAVM_FAIL");
    if (!strcmp(k, "append") && !strcmp(f, "names")) return true;          // names: another word for append
    return !strcmp(f, k);
}
static std::string bakPath() { const char *p = getenv("SAVM_BAK"); return p ? p : "savm_sim.bak"; }
static std::filesystem::path card() { const char *p = getenv("SAVM_CARD"); return p ? p : "savm_card"; }

// A card name as a host path: SAVES.BAK is $SAVM_BAK, SAVES.TMP <that>.tmp, SAVES.X <that>.x (BK1, UND, UTM);
// anything else is under $SAVM_CARD.
static std::string host(const char *name) {
    if (!strncmp(name, "SAVES.", 6)) {
        if (!strcmp(name + 6, "BAK")) return bakPath();
        std::string e = name + 6;
        for (char &c : e) if (c >= 'A' && c <= 'Z') c += 32;
        return bakPath() + "." + e;
    }
    return (card() / name).string();
}

static uint32_t mounts = 0;
static Result mount() {
    if (!mounts++ && !getenv("SAVM_BAK")) {     // the default backup files start afresh: script runs repeat exactly
        std::error_code ec;
        for (const char *e : { "", ".tmp", ".bk1", ".und", ".utm" }) std::filesystem::remove(bakPath() + e, ec);
    }
    if (!strcmp(env("SAVM_NOCARD"), "1") || fault("nocard")) return NO_CARD;
    if (!strcmp(env("SAVM_NOCARD"), "fat") || fault("nofat")) return NOT_FAT;
    if (!strcmp(env("SAVM_NOCARD"), "err") || fault("cardfail")) return IO;
    return OK;
}

static void promote(const std::string &tmp, const std::string &fin) {
    std::error_code ec;
    if (!std::filesystem::exists(fin, ec) && std::filesystem::exists(tmp, ec)) std::filesystem::rename(tmp, fin, ec);
}

static Result readOne(const std::string &p, uint8_t *pages, uint8_t np);

// Write a file as the board does: <tmp>, read back and compare, (bk1) <final> -> bk1 or remove <final>, rename.
// The fault "cut" stops after the old file is gone, as a power cut there would (only the .TMP is left).
static Result writeVia(const std::string &tmp, const std::string &fin, const uint8_t *a, uint16_t na,
                       const uint8_t *b, uint16_t nb, const char *bk1 = nullptr) {
    if (fault("io")) return IO;
    promote(tmp, fin);
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return IO;
    bool ok = (!na || fwrite(a, 1, na, f) == na) && (!nb || fwrite(b, 1, nb, f) == nb);
    if (fclose(f) != 0 || !ok) return IO;
    f = fopen(tmp.c_str(), "rb");
    if (!f) return IO;
    std::vector<uint8_t> back(na + nb + 1);
    size_t n = fread(back.data(), 1, back.size(), f);
    fclose(f);
    ok = n == (size_t)(na + nb) && (!na || !memcmp(back.data(), a, na)) && (!nb || !memcmp(back.data() + na, b, nb));
    if (!ok || fault("crc")) return IO;
    std::error_code ec;
    std::vector<uint8_t> old(PAGE * 2);
    if (bk1 && readOne(fin, old.data(), na > 12 ? a[12] : 0) == OK) {     // only a good old file replaces bk1
        std::filesystem::remove(host(bk1), ec);
        std::filesystem::rename(fin, host(bk1), ec);
    } else std::filesystem::remove(fin, ec);
    if (fault("cut")) return IO;
    ec.clear();
    std::filesystem::rename(tmp, fin, ec);
    return ec ? IO : OK;
}

Result writeRec(const char *fin, const char *tmp, const uint8_t *pages, uint8_t n, const char *bk1) {
    Result m = mount();
    if (m != OK) return m;
    if (strchr(fin, '/')) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(host(fin)).parent_path(), ec);
    }
    uint8_t head[HEAD];
    memcpy(head, TAG, 8);
    uint32_t crc = crc32(pages, PAGE * n);
    memcpy(head + 8, &crc, 4);
    head[12] = n; head[13] = 0; head[14] = 1; head[15] = 0;
    return writeVia(host(tmp), host(fin), head, HEAD, pages, (uint16_t)(PAGE * n), bk1);
}

static Result readOne(const std::string &p, uint8_t *pages, uint8_t np) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return NO_FILE;
    if (fault("io")) { fclose(f); return IO; }
    uint8_t head[HEAD];
    Result res = BAD_FILE;                  // read fine but not a backup (short, wrong tag, wrong CRC)
    size_t n = fread(head, 1, HEAD, f);
    if (ferror(f) || fault("read")) res = IO;
    else if (n == HEAD && !memcmp(head, TAG, 8) && head[12] == np && head[13] == 0) {
        n = fread(pages, 1, PAGE * np, f);
        if (ferror(f)) res = IO;
        else if (n == (size_t)(PAGE * np)) {
            uint32_t crc;
            memcpy(&crc, head + 8, 4);
            if (crc == crc32(pages, PAGE * np)) res = OK;
        }
    }
    fclose(f);
    return res;
}

Result readRec(const char *fin, const char *tmp, uint8_t *pages, uint8_t n) {
    Result m = mount();
    if (m != OK) return m;
    Result r = readOne(host(fin), pages, n);
    if (r == NO_FILE) r = readOne(host(tmp), pages, n);
    return r;
}

Result srcScan(uint8_t *pages, void (*cb)(const char *name, uint8_t n, Result r, const uint8_t *pages)) {
    Result m = mount();
    if (m != OK) return m;
    static const char *const fixed[3][2] = { { "SAVES.BAK", "SAVES.TMP" }, { "SAVES.BK1", nullptr }, { "SAVES.UND", "SAVES.UTM" } };
    for (int i = 0; i < 3; i++) {
        Result r = readOne(host(fixed[i][0]), pages, 2);
        if (r == NO_FILE && fixed[i][1]) r = readOne(host(fixed[i][1]), pages, 2);
        if (!i || r != NO_FILE) cb(fixed[i][0], 2, r, pages);
    }
    std::error_code ec;
    std::vector<std::filesystem::path> v;
    for (const auto &e : std::filesystem::directory_iterator(card() / "SAVES", ec)) {
        std::string x = e.path().extension().string();
        for (char &c : x) if (c >= 'a' && c <= 'z') c -= 32;
        if (!e.is_regular_file()) continue;
        if (x == ".SAV") v.push_back(e.path());
        else if (x == ".TMP" && !std::filesystem::exists(std::filesystem::path(e.path()).replace_extension(".SAV"), ec) &&
                 !std::filesystem::exists(std::filesystem::path(e.path()).replace_extension(".sav"), ec))
            v.push_back(e.path());                  // a .TMP alone (a write cut before its rename) is the .SAV
    }
    std::sort(v.begin(), v.end());
    for (const auto &p : v) {
        std::string n = std::filesystem::path(p).replace_extension(".SAV").filename().string();
        for (char &c : n) if (c >= 'a' && c <= 'z') c -= 32;
        cb(n.c_str(), 1, readOne(p.string(), pages, 1), pages);
    }
    return OK;
}

// ---- text files ----

static Result readOneText(const char *name, char *buf, uint16_t cap, uint16_t *len, bool *more) {
    FILE *f = fopen(host(name).c_str(), "rb");
    if (!f) return NO_FILE;
    if (fault("io")) { fclose(f); return IO; }
    size_t n = fread(buf, 1, cap, f);
    *more = fgetc(f) != EOF;
    fclose(f);
    if (*more) while (n && buf[n - 1] != 10) n--;            // a file over cap: whole lines of its start
    *len = (uint16_t)n;
    return OK;
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
    std::error_code ec;
    std::filesystem::create_directories(card(), ec);
    return writeVia(host(tmp), host(fin), (const uint8_t *)buf, len, nullptr, 0);
}

Result appendText(const char *fin, const char *tmp, const char *buf, uint16_t len) {
    Result m = mount();
    if (m != OK) return m;
    if (fault("io") || (fault("append") && !strcmp(fin, "SAVENAME.TXT"))) return IO;
    std::error_code ec;
    std::filesystem::create_directories(card(), ec);
    promote(host(tmp), host(fin));
    FILE *f = fopen(host(fin).c_str(), "ab");
    if (!f) return IO;
    bool ok = fwrite("\n", 1, 1, f) == 1 && fwrite(buf, 1, len, f) == len;
    return (fclose(f) == 0 && ok) ? OK : IO;
}

static Result findIn(const char *name, NameFinder &nf) {
    FILE *f = fopen((card() / name).string().c_str(), "rb");
    if (!f) return NO_FILE;
    if (fault("io")) { fclose(f); return IO; }
    for (int c; (c = fgetc(f)) != EOF; ) nf.feed((char)c);
    fclose(f);
    return OK;
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

// ---- the game scan (GAMES/*.CHG and one level of sub-folders, a few KB per call) ----

static chgscan::ChgScan sc;
static std::vector<std::filesystem::path> files;    // the simulator may use the heap; the board does not

struct Priv { uint32_t mount; FILE *f; uint32_t left; uint32_t next; };   // next: the next index in files
static_assert(sizeof(Priv) <= sizeof(Scan::priv), "Scan::priv too small");

// No name test for macOS "._X.CHG" junk, as on the board (where it lists as "_X~1.CHG"): the header check drops it.
static bool isChg(const std::filesystem::path &p) {
    std::string e = p.extension().string();
    return e.size() == 4 && (e[1] | 32) == 'c' && (e[2] | 32) == 'h' && (e[3] | 32) == 'g';
}

// GAMELIST.TXT in $SAVM_CARD. listBegin does not "mount" (it runs inside a scan, as on the board).
static FILE *lf = nullptr;
Result listBegin(const char *name) {
    if (fault("io")) return IO;
    lf = fopen((card() / name).string().c_str(), "wb");
    return lf ? OK : IO;
}
Result listAdd(const char *line, uint16_t len) {
    if (!lf) return IO;
    if (fwrite(line, 1, len, lf) == len) return OK;
    fclose(lf); lf = nullptr;
    return IO;
}
void listEnd() { if (lf) fclose(lf); lf = nullptr; }
Result listFeed(char *ign, uint16_t cap, uint16_t *ilen, bool *imore, void (*feed)(char c)) {
    Result m = mount();
    if (m != OK) return m;
    if (textIn("IGNORE.TXT", "IGNORE.TMP", ign, cap, ilen, imore) == IO) return IO;
    mounts--;                               // one mount for both, as on the board
    return fileFeed("GAMELIST.TXT", feed);
}
Result fileFeed(const char *path, void (*feed)(char c)) {
    Result m = mount();
    if (m != OK) return m;
    if (fault("io")) return IO;
    FILE *f = fopen((card() / path).string().c_str(), "rb");
    if (!f) return NO_FILE;
    int c;
    while ((c = fgetc(f)) != EOF) feed((char)c);
    fclose(f);
    return OK;
}

void (*scanSkip)(const char *path, const char *why) = nullptr;     // debug builds only use it

Result scanBegin(Scan &s) {
    s.fileIndex = 0;
    s.skipDirs = s.skipFiles = s.badFiles = 0;
    s.state = 0;
    Priv *p = (Priv *)s.priv;
    p->f = nullptr;
    p->next = 0;
    files.clear();
    Result m = mount();
    if (m != OK) return m;
    p->mount = mounts;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path games = card() / "GAMES";
    if (!fs::is_directory(games, ec)) return NO_GAMES;
    if (fault("io")) return IO;
    for (const fs::directory_entry &e : fs::directory_iterator(games, ec)) {
        if (e.is_directory()) {
            for (const fs::directory_entry &f : fs::directory_iterator(e.path(), ec)) {
                if (f.is_directory()) {             // a second level (GAMES/OTHER/CARDS); deeper is not scanned
                    for (const fs::directory_entry &g : fs::directory_iterator(f.path(), ec)) {
                        if (g.is_regular_file() && isChg(g.path())) files.push_back(g.path());
                        else if (g.is_directory()) {    // too deep: counted, with the .CHG files right in it
                            s.skipDirs++;
                            for (const fs::directory_entry &h : fs::directory_iterator(g.path(), ec))
                                if (h.is_regular_file() && isChg(h.path())) s.skipFiles++;
                        }
                    }
                } else if (f.is_regular_file() && isChg(f.path())) files.push_back(f.path());
            }
        } else if (e.is_regular_file() && isChg(e.path())) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    s.state = 1;
    return OK;
}

void scanEnd(Scan &s) {
    Priv *p = (Priv *)s.priv;
    if (p->f) { fclose(p->f); p->f = nullptr; }
    s.state = 0;
}

Result scanNext(Scan &s, ScanItem &item) {
    Priv *p = (Priv *)s.priv;
    if (s.state && p->mount != mounts) { scanEnd(s); return IO; }     // remounted: the folders are gone
    if (p->f) {
        for (uint16_t done = 0; p->left && done < SCAN_STEP; ) {
            uint16_t n = p->left < chgscan::CHUNK ? (uint16_t)p->left : chgscan::CHUNK;
            if (fread(sc.chunk(), 1, n, p->f) != n) { scanEnd(s); return IO; }
            sc.feed(n);
            p->left -= n;
            done += n;
        }
        if (p->left) return BUSY;
        fclose(p->f); p->f = nullptr;
        sc.finish();
        item.nCodes = sc.nCodes;
        memcpy(item.codes, sc.codes, sizeof(item.codes));
        memcpy(item.score, sc.score, sizeof(item.score));
        return OK;
    }
    while (s.state && p->next < files.size()) {
        const std::filesystem::path &path = files[p->next++];
        FILE *f = fopen(path.string().c_str(), "rb");
        if (!f) { scanEnd(s); return IO; }
        uint32_t len = 0;
        sc.begin();
        bool ok = fread(sc.chunk(), 1, chgscan::CHUNK, f) == chgscan::CHUNK &&
                  chgscan::header(sc.chunk(), item.title, &len);
        std::error_code ec;
        uintmax_t size = std::filesystem::file_size(path, ec);
        ok = ok && !ec && len > 0 && len <= size - chgscan::CHUNK;
        if (!ok) {
            fclose(f);
            if (scanSkip) scanSkip(path.filename().string().c_str(), "bad CHG1 header or length");
            if (path.filename().string().rfind("._", 0) != 0 && !ec && size >= chgscan::CHUNK) s.badFiles++;   // as the board
            continue;
        }
        std::string fn = path.filename().string();             // as FatFs shows it without LFN: 8.3, upper case
        for (char &c : fn) if (c >= 'a' && c <= 'z') c -= 32;
        strncpy(item.file, fn.c_str(), sizeof item.file - 1);
        item.file[sizeof item.file - 1] = 0;
        std::string rel = path.lexically_relative(card() / "GAMES").generic_string();
        for (char &c : rel) if (c >= 'a' && c <= 'z') c -= 32;
        strncpy(item.path, rel.c_str(), sizeof item.path - 1);
        item.path[sizeof item.path - 1] = 0;
        sc.begin();
        p->f = f;
        p->left = len;
        s.fileIndex++;                                          // only a valid header counts
        return BUSY;
    }
    if (s.state && fault("scan")) { scanEnd(s); return IO; }
    s.state = 0;
    return DONE;
}

}  // namespace sdio
#endif
