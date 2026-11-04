// Host tests of CHSd's FAT reader (src/Fat.cpp) against card images made by
// run_tests.py with tools/fatimg.py, whose layout is the ground truth, and
// of the simulator's pretend card (host/VCard.h).
//
//   test_fat IMAGE SPEC     SPEC: what is on the image (run_tests.py writes it)
//   test_fat --none         no card in the slot
//   test_fat --vcard        files of many sizes on a VCard
//
// SPEC lines (an 11-character name has its spaces written as '_'):
//   mount RC                         what mount() must return
//   file NAME SIZE SEED N lba n ...  a root file, its contents (SEED) and runs
//   dir NAME                         a folder in the root
//   match DIR PATTERN COUNT NAME...  the files a pattern must find in a folder
//   missing NAME                     no root file by this name
//   chain NAME RC                    runs() of this root file must fail with RC
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "../src/Fat.h"
#include "../src/SdSpi.h"
#include "../host/VCard.h"

static long checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { checks++; long long x_ = (long long)(a), y_ = (long long)(b); \
    if (x_ != y_) { fails++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, x_, y_); } } while (0)

// --- The card: an image in memory, or a VCard over a file in memory ----------
static std::vector<uint8_t> image, vfile;
static vcard::Card vc;
static bool useVc;
static long reads, failAfter = -1;      // failAfter >= 0: that many more reads work

namespace sd {
bool init() { return (useVc || !image.empty()) && failAfter != 0; }
bool read(uint32_t lba, uint8_t *dst) {
    if (failAfter == 0) return false;
    if (failAfter > 0) failAfter--;
    reads++;
    if (useVc) {
        if (lba >= vc.blocks()) return false;
        long k = vc.read(lba, dst);
        if (k >= 0) {
            memset(dst, 0, 512);
            memcpy(dst, vfile.data() + k * 512, std::min<size_t>(512, vfile.size() - k * 512));
        }
        return true;
    }
    if ((uint64_t)lba * 512 + 512 > image.size()) return false;
    memcpy(dst, image.data() + (size_t)lba * 512, 512);
    return true;
}
}  // namespace sd

alignas(4) static uint8_t buf[512];

// The contents run_tests.py gives a file.
static uint8_t byteAt(uint32_t i, uint32_t seed) { return (uint8_t)(i * 7 + seed * 13 + (i >> 9)); }

static std::string name11(const char *s) {
    std::string n = s;
    for (auto &c : n) if (c == '_') c = ' ';
    return n;
}

struct XFile { std::string name; uint32_t size, seed; std::vector<fat::Run> runs; };
struct XMatch { std::string dir, pattern; std::vector<std::string> names; };

// Every block of a file through fat::read, against what was written.
static void readAll(const fat::Run *r, uint32_t n, uint32_t size, uint32_t seed) {
    uint32_t blocks = (size + 511) / 512;
    for (uint32_t k = 0; k < blocks; k++) {
        CHECK(fat::read(r, n, k, buf));
        uint32_t bad = 0;
        for (uint32_t j = 0; j < 512 && k * 512 + j < size; j++) bad += buf[j] != byteAt(k * 512 + j, seed);
        CHECK_EQ(bad, 0);
    }
    CHECK(!fat::read(r, n, blocks, buf));
}

static int runImage(const char *imagePath, const char *specPath) {
    FILE *f = fopen(imagePath, "rb");
    if (!f) { printf("cannot open %s\n", imagePath); return 2; }
    fseek(f, 0, SEEK_END);
    image.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(image.data(), 1, image.size(), f) != image.size()) return 2;
    fclose(f);

    int mountRc = 0;
    std::vector<XFile> files;
    std::vector<std::string> dirs, missing;
    std::vector<XMatch> matches;
    std::vector<std::pair<std::string, int>> chains;
    f = fopen(specPath, "r");
    if (!f) return 2;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *tok = strtok(line, " \r\n");
        if (!tok) continue;
        std::string kind = tok;
        auto next = [] { return strtok(nullptr, " \r\n"); };
        if (kind == "mount") mountRc = atoi(next());
        else if (kind == "file") {
            XFile x;
            x.name = name11(next());
            x.size = (uint32_t)strtoul(next(), nullptr, 10);
            x.seed = (uint32_t)strtoul(next(), nullptr, 10);
            int n = atoi(next());
            for (int i = 0; i < n; i++) {
                fat::Run r;
                r.lba = (uint32_t)strtoul(next(), nullptr, 10);
                r.blocks = (uint32_t)strtoul(next(), nullptr, 10);
                x.runs.push_back(r);
            }
            files.push_back(x);
        } else if (kind == "dir") dirs.push_back(name11(next()));
        else if (kind == "match") {
            XMatch m;
            m.dir = name11(next());
            m.pattern = name11(next());
            int n = atoi(next());
            for (int i = 0; i < n; i++) m.names.push_back(name11(next()));
            matches.push_back(m);
        } else if (kind == "missing") missing.push_back(name11(next()));
        else if (kind == "chain") {
            std::string n = name11(next());
            chains.push_back({n, atoi(next())});
        }
    }
    fclose(f);

    // Nothing works before a mount.
    fat::File file;
    CHECK_EQ(fat::find("ANYTHING   ", file, buf), fat::E_NOTFOUND);

    CHECK_EQ(fat::mount(buf), mountRc);
    if (mountRc) {
        fat::Run r[4];
        CHECK_EQ(fat::open("ANYTHING   ", r, 4, buf), 0);
        CHECK_EQ(fat::find("ANYTHING   ", file, buf), fat::E_NOTFOUND);
        printf("%ld checks, %ld failed\n", checks, fails);
        return fails ? 1 : 0;
    }

    long walkReads = 0;
    reads = 0;
    for (const XFile &x : files) {
        CHECK_EQ(fat::find(x.name.c_str(), file, buf), fat::OK);
        CHECK_EQ(file.size, x.size);
        fat::Run r[16];
        int8_t n = fat::runs(file, r, 16, buf);
        CHECK_EQ(n, (int)x.runs.size());
        for (int i = 0; i < n && i < (int)x.runs.size(); i++) {
            CHECK_EQ(r[i].lba, x.runs[i].lba);
            CHECK_EQ(r[i].blocks, x.runs[i].blocks);
        }
        if (n > 1) {
            CHECK_EQ(fat::runs(file, r, (uint8_t)(n - 1), buf), fat::E_FRAG);
            CHECK_EQ(fat::open(x.name.c_str(), r, (uint8_t)(n - 1), buf), 0);
        }
        // open() is the same in one call.
        fat::Run r2[16];
        CHECK_EQ(fat::open(x.name.c_str(), r2, 16, buf), n);
        if (n > 0) readAll(r2, (uint32_t)n, x.size, x.seed);
        // '?' stands for any character.
        std::string wild = x.name;
        wild[0] = wild[10] = '?';
        CHECK_EQ(fat::find(wild.c_str(), file, buf), fat::OK);
    }
    walkReads = reads;
    for (const std::string &d : dirs) {
        fat::File dir;
        CHECK_EQ(fat::folder(d.c_str(), dir, buf), fat::OK);
        CHECK(dir.cluster >= 2);
        CHECK_EQ(fat::find(d.c_str(), file, buf), fat::E_NOTFOUND);     // a folder is not a file
    }
    for (const XFile &x : files) {
        fat::File dir;
        CHECK_EQ(fat::folder(x.name.c_str(), dir, buf), fat::E_NOTFOUND);  // a file is not a folder
    }
    for (const XMatch &m : matches) {
        fat::File dir;
        CHECK_EQ(fat::folder(m.dir.c_str(), dir, buf), fat::OK);
        std::vector<std::string> got;
        for (uint8_t k = 0; k < 40; k++) {
            char name[11];
            int8_t rc = fat::match(dir, m.pattern.c_str(), k, file, name, buf);
            if (rc) { CHECK_EQ(rc, fat::E_NOTFOUND); break; }
            got.push_back(std::string(name, 11));
            fat::Run r[16];
            CHECK(fat::runs(file, r, 16, buf) >= 0);
        }
        std::vector<std::string> want = m.names;
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        CHECK(got == want);
        if (got != want) {
            printf("  match %s/%s: got", m.dir.c_str(), m.pattern.c_str());
            for (auto &g : got) printf(" [%s]", g.c_str());
            printf("\n");
        }
    }
    // list(): the root holds the root files and the folders (no label, no
    // long-name parts); a folder's listing is its every-name match (no "."
    // or "..", no hidden file). Each entry agrees with find()/folder().
    {
        struct Seen { std::vector<std::string> names; std::vector<fat::File> f; std::vector<bool> dir; };
        auto collect = [](const char *n, const fat::File &f, bool isDir, void *ctx) {
            Seen &s = *(Seen *)ctx;
            s.names.push_back(std::string(n, 11));
            s.f.push_back(f);
            s.dir.push_back(isDir);
            return true;
        };
        fat::File rootDir;
        CHECK_EQ(fat::root(rootDir), fat::OK);
        Seen seen;
        CHECK_EQ(fat::list(rootDir, collect, &seen, buf), fat::OK);
        std::vector<std::string> want;
        for (const XFile &x : files) want.push_back(x.name);
        for (const std::string &d : dirs) want.push_back(d);
        std::vector<std::string> got = seen.names;
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        if (!files.empty()) CHECK(got == want);       // (the chain cards list only their broken file)
        for (size_t i = 0; i < seen.names.size(); i++) {
            fat::File g;
            if (seen.dir[i]) {
                CHECK_EQ(fat::folder(seen.names[i].c_str(), g, buf), fat::OK);
            } else {
                CHECK_EQ(fat::find(seen.names[i].c_str(), g, buf), fat::OK);
                CHECK_EQ(g.size, seen.f[i].size);
            }
            CHECK_EQ(g.cluster, seen.f[i].cluster);
        }
        // Stopping early: fn's false ends the walk at once.
        int calls = 0;
        auto stop = [](const char *, const fat::File &, bool, void *ctx) { ++*(int *)ctx; return false; };
        CHECK_EQ(fat::list(rootDir, stop, &calls, buf), fat::OK);
        CHECK_EQ(calls, (int)(seen.names.empty() ? 0 : 1));
        for (const XMatch &m : matches) {
            if (m.pattern != "???????????") continue;
            fat::File dir;
            CHECK_EQ(fat::folder(m.dir.c_str(), dir, buf), fat::OK);
            Seen sub;
            CHECK_EQ(fat::list(dir, collect, &sub, buf), fat::OK);
            std::vector<std::string> g2 = sub.names, w2 = m.names;
            std::sort(g2.begin(), g2.end());
            std::sort(w2.begin(), w2.end());
            CHECK(g2 == w2);
            for (bool d : sub.dir) CHECK(!d);
        }
    }
    for (const std::string &n : missing) CHECK_EQ(fat::find(n.c_str(), file, buf), fat::E_NOTFOUND);
    for (auto &c : chains) {
        CHECK_EQ(fat::find(c.first.c_str(), file, buf), fat::OK);
        fat::Run r[16];
        CHECK_EQ(fat::runs(file, r, 16, buf), c.second);
    }

    // The card pulled after k reads, for every k: every call returns (no
    // hang, nothing read out of bounds) and says it failed or gives the
    // right answer.
    for (long k = 0; k < walkReads; k += (walkReads > 600 ? 5 : 1)) {
        failAfter = k;
        int8_t rc = fat::mount(buf);
        for (const XFile &x : files) {
            if (rc) break;
            fat::Run r[16];
            uint8_t n = fat::open(x.name.c_str(), r, 16, buf);
            if (!n) break;
            CHECK_EQ(n, (int)x.runs.size());
            for (uint32_t b = 0; b < (x.size + 511) / 512; b++)
                if (!fat::read(r, (uint32_t)n, b, buf)) break;
        }
        failAfter = -1;
    }
    CHECK_EQ(fat::mount(buf), 0);
    printf("%ld checks, %ld failed\n", checks, fails);
    return fails ? 1 : 0;
}

// A VCard around files of many sizes: mount, find, two runs, every byte.
static int runVcard() {
    useVc = true;
    static const uint32_t sizes[] = {0, 1, 511, 512, 513, 4096, 4097, 8192, 40000, 70000, 4194816};
    for (uint32_t size : sizes) {
        vfile.resize(size);
        for (uint32_t i = 0; i < size; i++) vfile[i] = byteAt(i, 5);
        vc.setup("out/Words.dic", size);
        fat::Run r[4];
        int n = fat::open("WORDS   DIC", r, 4, buf);
        uint32_t clusters = (size + 4095) / 4096;
        CHECK_EQ(n, clusters == 0 ? 0 : clusters == 1 ? 1 : 2);
        if (n > 0) readAll(r, (uint32_t)n, size, 5);
        if (n == 2) CHECK_EQ(fat::open("WORDS   DIC", r, 1, buf), 0);
        fat::File f;
        CHECK_EQ(fat::find("CHSD SIM   ", f, buf), fat::E_NOTFOUND);      // the label is not a file
    }
    vc.setup("C:\\x\\PHRASES.BNK", 39424);
    vfile.assign(39424, 0);
    fat::File f;
    CHECK_EQ(fat::mount(buf), 0);
    CHECK_EQ(fat::find("PHRASES BNK", f, buf), 0);
    CHECK_EQ(f.size, 39424);
    printf("%ld checks, %ld failed (vcard)\n", checks, fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--none")) {
        fat::Run r[4];
        CHECK_EQ(fat::open("WORDS   DIC", r, 4, buf), 0);
        CHECK_EQ(fat::mount(buf), fat::E_READ);
        printf("%ld checks, %ld failed\n", checks, fails);
        return fails ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--vcard")) return runVcard();
    if (argc != 3) return 2;
    return runImage(argv[1], argv[2]);
}
