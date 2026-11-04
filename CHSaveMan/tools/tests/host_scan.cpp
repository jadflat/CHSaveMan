// Host test for chgscan.h on real .CHG files. From the project root in Git Bash:
//   export PATH=<w64devkit>/bin:$PATH
//   g++ -std=c++17 -O1 -Wall -o CHSaveMan/tools/tests/out/host_scan.exe CHSaveMan/tools/tests/host_scan.cpp
//   CHSaveMan/tools/tests/out/host_scan.exe CHSaveMan/tools/tests/scan_truth.txt <root folder that holds Arduino/> [-v]
// scan_truth.txt (a local list, not in the repository: write one for your own .CHG files):
// "<code or -> <path under the root>" per line (# comments). A file passes when the scan
// gives exactly that one code (or none for -); "top" counts files whose first code is right. Missing
// files are skipped. Every file is also scanned with chunk edges at other places: same codes, same order.
// Prints PASS or FAIL (FAIL only for chunk-edge differences and the synthetic tests: accuracy is reported).
#include "../../chgscan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

using chgscan::ChgScan;

static int fails = 0;

static bool load(const std::string &path, std::vector<uint8_t> &d) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    return true;
}

// Scan the payload, the first chunk being `first` bytes (then CHUNK-sized ones): returns the codes,
// each with its score ("CODE:score").
static std::string scan(const uint8_t *p, uint32_t len, uint16_t first, bool scores = false) {
    static ChgScan s;
    s.begin();
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = off == 0 ? first : chgscan::CHUNK;
        if (n > len - off) n = len - off;
        memcpy(s.chunk(), p + off, n);
        s.feed((uint16_t)n);
        off += n;
    }
    s.finish();
    std::string out;
    for (uint8_t i = 0; i < s.nCodes; i++) {
        if (i) out += ',';
        out.append(s.codes[i], 4);
        if (scores) out += ":" + std::to_string(s.score[i]);
    }
    return out;
}

// Write a lui/addi pair building v in register r at p[at] (gap: bytes of c.nop between them).
static void pair(std::vector<uint8_t> &p, uint32_t at, uint32_t v, uint8_t r, uint8_t gap) {
    uint32_t hi = (v + 0x800) & 0xFFFFF000u, lo = (v - hi) & 0xFFF;
    uint32_t lui = hi | (r << 7) | 0x37, addi = (lo << 20) | (r << 15) | (r << 7) | 0x13;
    for (int i = 0; i < 4; i++) p[at + i] = (uint8_t)(lui >> (8 * i));
    for (int i = 0; i < gap; i += 2) { p[at + 4 + i] = 0x01; p[at + 5 + i] = 0x00; }
    for (int i = 0; i < 4; i++) p[at + 4 + gap + i] = (uint8_t)(addi >> (8 * i));
}
static void put32(std::vector<uint8_t> &p, uint32_t at, uint32_t w) {
    for (int i = 0; i < 4; i++) p[at + i] = (uint8_t)(w >> (8 * i));
}

static void synthetic() {
    const uint32_t CHIX = 'C' | 'H' << 8 | 'I' << 16 | (uint32_t)'X' << 24;
    const uint32_t LW_A3_500_A4 = (0x500u << 20) | (14 << 15) | (2 << 12) | (13 << 7) | 0x03;   // lw a3,0x500(a4)
    const uint32_t BNE_A3_A5 = (15u << 20) | (13 << 15) | (1 << 12) | (8 << 7) | 0x63;          // bne a3,a5,+8
    // The record check across every chunk edge position: lw; lui; c.nop; addi; bne.
    for (uint32_t at = 496; at <= 516; at += 2) {
        std::vector<uint8_t> p(1536, 0);
        put32(p, at - 4, LW_A3_500_A4);
        pair(p, at, CHIX, 15, 2);
        put32(p, at + 10, BNE_A3_A5);
        std::string g = scan(p.data(), (uint32_t)p.size(), chgscan::CHUNK);
        if (g != "CHIX") { printf("FAIL synthetic record check at %u [%s]\n", at, g.c_str()); fails++; }
    }
    // The save call: lui a0; c.li a1,1; addi a0; jal ra.
    {
        std::vector<uint8_t> p(1024, 0);
        pair(p, 100, CHIX, 10, 2);
        p[104] = 0x85; p[105] = 0x45;                               // c.li a1,1
        put32(p, 110, 0x000000EFu | (0x100u << 20));                // jal ra,+0x100
        std::string g = scan(p.data(), (uint32_t)p.size(), chgscan::CHUNK);
        printf("%s synthetic save call [%s]\n", g == "CHIX" ? "ok  " : "FAIL", g.c_str());
        if (g != "CHIX") fails++;
    }
    // The same pair with nothing reading it: no code.
    {
        std::vector<uint8_t> p(1024, 0);
        pair(p, 100, CHIX, 15, 0);
        std::string g = scan(p.data(), (uint32_t)p.size(), chgscan::CHUNK);
        printf("%s synthetic unused pair [%s]\n", g.empty() ? "ok  " : "FAIL", g.c_str());
        if (!g.empty()) fails++;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("host_scan <truth file> <root> [-v]\n"); return 2; }
    bool verbose = argc > 3;
    std::string root = argv[2];
    FILE *t = fopen(argv[1], "r");
    if (!t) { printf("FAIL cannot read %s\n", argv[1]); return 1; }
    char line[512];
    int n = 0, exact = 0, top = 0, skipped = 0;
    while (fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        size_t sp = l.find(' ');
        std::string want = l.substr(0, sp), file = l.substr(sp + 1);
        if (want == "-") want.clear();
        std::vector<uint8_t> d;
        char title[33];
        uint32_t len = 0;
        if (!load(root + "/" + file, d) || d.size() < 512) { skipped++; continue; }
        if (!chgscan::header(d.data(), title, &len) || len == 0 || len > d.size() - 512) {
            printf("FAIL %s: bad header\n", file.c_str()); fails++; continue;
        }
        std::string got = scan(d.data() + 512, len, chgscan::CHUNK);
        n++;
        bool ex = got == want, tp = want.empty() ? got.empty() : got.compare(0, 4, want) == 0;
        exact += ex; top += tp;
        if (verbose || !ex)
            printf("%s %-24s want %-4s got [%s]  %s\n", ex ? "ok  " : tp ? "top " : "MISS", title,
                   want.empty() ? "-" : want.c_str(), scan(d.data() + 512, len, chgscan::CHUNK, true).c_str(),
                   file.c_str());
        static const uint16_t firsts[] = { 1, 2, 3, 7, 30, 31, 33, 100, 257, 511 };
        for (uint16_t f : firsts) {
            std::string g = scan(d.data() + 512, len, f);
            if (g != got) { printf("FAIL %s: first chunk %u gives [%s]\n", file.c_str(), f, g.c_str()); fails++; }
        }
    }
    fclose(t);
    synthetic();
    printf("%d files (%d skipped): exactly right %d, right code first %d\n", n, skipped, exact, top);
    printf("sizeof(ChgScan) = %u\n", (unsigned)sizeof(ChgScan));
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails != 0;
}
