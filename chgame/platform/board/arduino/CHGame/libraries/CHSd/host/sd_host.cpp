// The simulator's SD card (the stand-in for CHSd's src/SdSpi.cpp;
// tools/chsim/chsim.py compiles it for a sketch that includes CHSd): the file
// named by $CHSD_CARD is in the slot. A FAT image (*.img, e.g. from CHSd's
// tools/fatimg.py) is served as the card's blocks; any other file is put on
// a pretend FAT16 card of its own (VCard.h), so the game's FAT code runs in
// the simulator just as on the board. With the variable unset there is no
// card. The debug protocol's eject command pulls it out and puts it back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim.h"
#include "VCard.h"
#include "SdSpi.h"

static FILE *s_card;
static bool s_out, s_image;         // pulled out; a whole image, not a file to wrap
static vcard::Card s_vc;

static FILE *card() {
    const char *path = getenv("CHSD_CARD");
    if (!s_card && path && (s_card = fopen(path, "rb"))) {
        size_t n = strlen(path);
        s_image = n > 4 && (!strcmp(path + n - 4, ".img") || !strcmp(path + n - 4, ".IMG"));
        fseek(s_card, 0, SEEK_END);
        s_vc.setup(path, (uint32_t)ftell(s_card));
    }
    return s_out ? nullptr : s_card;
}

uint32_t sim_cardBlocks() {
    FILE *f = card();
    if (!f) return 0;
    if (!s_image) return s_vc.blocks();
    fseek(f, 0, SEEK_END);
    return (uint32_t)(ftell(f) / 512);
}

void sim_cardEject(bool out) { s_out = out; }

namespace sd {

bool init() { return card() != nullptr; }

// A block, taking `us` of virtual time.
static bool fetch(uint32_t lba, uint8_t *dst, uint32_t us) {
    FILE *f = card();
    if (!f || lba >= sim_cardBlocks()) return false;
    sim_advance(us);
    long k = s_image ? (long)lba : s_vc.read(lba, dst);
    if (k < 0) return true;
    memset(dst, 0, 512);            // (a file's last block, past its end)
    return !fseek(f, k * 512, SEEK_SET) && fread(dst, 1, 512, f) > 0;
}

bool read(uint32_t lba, uint8_t *dst) {
    return fetch(lba, dst, 900);    // about what a polled block read takes on the board
}

// One command's latency, then each block at the 24 MHz wire rate; the
// board's fn runs while the next block arrives, so only the wire is timed.
bool stream(uint32_t lba, uint32_t n, uint8_t *buf0, uint8_t *buf1, BlockFn fn, void *ctx) {
    if (!n) return true;
    sim_advance(600);
    for (uint32_t k = 0; k < n; k++) {
        uint8_t *b = (k & 1) ? buf1 : buf0;
        if (!fetch(lba + k, b, 170)) return false;
        fn(b, ctx);
    }
    return true;
}

}  // namespace sd
