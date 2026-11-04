// src/SdSpi.cpp (init and read) against a model of an SD card in SPI mode,
// behind the registers in tests/spi/Arduino.h. The model keeps what a real
// card keeps across an MCU reset (it stays powered): its state and whether
// CRC checking is on. CHSDtoUSB turns checking on, and the model's CMD0 does
// not turn it off, the worst case (the bootloader's test/native/sd_model.c
// assumes the same). It is also a strict card about N_RC: a command must not
// start on the byte right after the last byte of a response (some cards,
// such as a SanDisk 32 GB "SK32G", take it misaligned and stop answering).
// tests/run_tests.py builds and runs it.
#include <stdio.h>
#include <string.h>
#include "Arduino.h"
#include "SdSpi.h"

SpiRegs t_spi;
GpioRegs t_gpioa, t_gpiob;
DmaRegs t_dma;
DmaChannel t_ch2, t_ch3;

static uint64_t now_us;
uint32_t micros() { return (uint32_t)now_us; }

static uint8_t crc7(const uint8_t *p) {
    uint8_t c = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t d = p[i];
        for (int b = 0; b < 8; b++, d <<= 1) {
            c <<= 1;
            if ((d ^ c) & 0x80) c ^= 0x09;
        }
    }
    return (uint8_t)(c << 1 | 1);
}

static uint8_t pattern(uint32_t lba, uint32_t i) { return (uint8_t)(lba * 7 + i * 13 + (i >> 8)); }

struct Card {
    enum Type { NONE, SDSC1, SDSC2, SDHC } type = NONE;
    enum State { POWERUP, IDLE, READY } state = POWERUP;
    bool crcOn = false, acmd = false;
    int acmd41Left = 0;
    uint32_t clocksCsHigh = 0, badCrc = 0, fastIdent = 0, conflicts = 0, noGap = 0, blocks = 1024;
    bool justAnswered = false;                              // the last byte out ended a response
    uint8_t cmd[6];
    int cmdLen = 0;
    uint8_t out[600];
    int outLen = 0, outPos = 0;

    void insert(Type t) { *this = Card(); type = t; power(); }
    void power() { state = POWERUP; clocksCsHigh = 0; cmdLen = outLen = outPos = 0; acmd = false; acmd41Left = 3; }
    void q(uint8_t b) { if (outLen < (int)sizeof out) out[outLen++] = b; }
    void q32(uint32_t v) { for (int sh = 24; sh >= 0; sh -= 8) q((uint8_t)(v >> sh)); }

    void command(unsigned br) {
        uint8_t c = cmd[0] & 0x3F, idle = state == READY ? 0x00 : 0x01;
        uint32_t arg = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
        bool a = acmd;
        acmd = false;
        outLen = outPos = 0;
        if (state != READY && br < 6) fastIdent++;          // BR 6 and 7: under 400 kHz
        if (state == POWERUP && (c != 0 || clocksCsHigh < 74)) return;
        // CMD0 and CMD8 are always checked; everything else once CMD59 has
        // turned checking on. A refused command answers "CRC error".
        if ((c == 0 || c == 8 || crcOn) && cmd[5] != crc7(cmd)) {
            badCrc++;
            q(0xFF); q(idle | 0x08);
            return;
        }
        q(0xFF);                                            // N_CR: one byte
        if (a && c == 41) {
            if (acmd41Left > 0) { acmd41Left--; q(0x01); }
            else { state = READY; q(0x00); }
            return;
        }
        switch (c) {
        case 0: state = IDLE; acmd41Left = 3; q(0x01); break;
        case 8:
            if (type == SDSC1) { q(0x05); break; }          // illegal: a v1 card
            q(idle); q32(arg & 0xFFF);
            break;
        case 55: acmd = true; q(idle); break;
        case 58: q(idle); q32((state == READY ? 0x80000000u : 0) | (type == SDHC ? 0x40000000u : 0) | 0x00FF8000u); break;
        case 16: q(idle); break;
        case 59: crcOn = arg & 1; q(idle); break;
        case 17: {
            if (state != READY) { q(idle | 0x04); break; }
            if (type != SDHC && (arg & 511)) { q(0x20); break; }   // address error
            uint32_t lba = type == SDHC ? arg : arg >> 9;
            if (lba >= blocks) { q(0x40); break; }
            q(0x00); q(0xFF); q(0xFF); q(0xFE);
            for (uint32_t i = 0; i < 512; i++) q(pattern(lba, i));
            q(0x12); q(0x34);                               // CRC16, not checked
            break;
        }
        default: q(idle | 0x04); break;
        }
    }

    uint8_t xfer(uint8_t in, bool csLow, unsigned br) {
        if (type == NONE) return 0xFF;
        if (!csLow) {
            if (clocksCsHigh < 1000) clocksCsHigh += 8;
            justAnswered = false;
            return 0xFF;                                    // DO released: the pull-up
        }
        bool noRc = justAnswered;
        justAnswered = outPos + 1 == outLen;
        uint8_t o = outPos < outLen ? out[outPos++] : 0xFF;
        if (cmdLen == 0) {
            if ((in & 0xC0) == 0x40 && noRc) noGap++;       // N_RC: 8 clocks at least
            if ((in & 0xC0) == 0x40) cmd[cmdLen++] = in;
        } else {
            cmd[cmdLen++] = in;
            if (cmdLen == 6) { cmdLen = 0; command(br); }
        }
        return o;
    }
};

static Card card;

SpiData &SpiData::operator=(uint32_t b) {
    unsigned br = (t_spi.CTLR1 >> 3) & 7;
    bool csLow = !(t_gpiob.OUTDR & (1u << 11));
    if (csLow && !(t_gpioa.OUTDR & (1u << 4))) card.conflicts++;   // the panel selected too
    now_us += 1 + (8u << (br + 1)) / 48;                // a byte at 48 MHz / 2^(BR+1)
    last = card.xfer((uint8_t)b, csLow, br);
    return *this;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("   FAIL %s: ", name); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

// A reset of the MCU, not of the card: the pins as CHGfx leaves them.
static void mcuReset() {
    t_spi = SpiRegs();
    t_spi.CTLR1 = (1u << 2) | (1u << 8) | (1u << 9) | (1u << 6);   // CHGfx's: master, BR 0, enabled
    t_gpioa.OUTDR = 0;                                  // the panel selected
    t_gpiob.OUTDR = 1u << 11;                           // the card not
}

static void initAndRead(const char *name, bool expectOk) {
    mcuReset();
    uint16_t lcd = t_spi.CTLR1;
    uint64_t t0 = now_us;
    bool ok = sd::init();
    CHECK(ok == expectOk, "init %d, expected %d", ok, expectOk);
    CHECK(t_spi.CTLR1 == lcd, "SPI1 not handed back as CHGfx left it");
    CHECK(card.conflicts == 0, "the panel and the card selected together");
    CHECK(card.fastIdent == 0, "identification above 400 kHz");
    CHECK(card.noGap == 0, "%u commands without N_RC", (unsigned)card.noGap);
    CHECK(card.badCrc == 0, "%u commands refused for their CRC", (unsigned)card.badCrc);
    if (!expectOk) {
        CHECK(now_us - t0 < 20000, "an empty slot took %u us", (unsigned)(now_us - t0));
        return;
    }
    CHECK(!card.crcOn, "card left with CRC checking on");
    uint8_t buf[512];
    static const uint32_t lbas[] = {0, 5, 1023};
    for (uint32_t lba : lbas) {
        memset(buf, 0, sizeof buf);
        bool r = sd::read(lba, buf);
        bool same = true;
        for (uint32_t i = 0; i < 512; i++) same &= buf[i] == pattern(lba, i);
        CHECK(r && same, "block %u", (unsigned)lba);
    }
    CHECK(!sd::read(card.blocks, buf), "a block past the end reads");
}

int main() {
    struct { const char *name; Card::Type type; bool crcOn; } cases[] = {
        {"SDHC", Card::SDHC, false},
        {"SDSC v2", Card::SDSC2, false},
        {"SDSC v1", Card::SDSC1, false},
        {"SDHC with CRC checking on", Card::SDHC, true},
        {"SDSC v1 with CRC checking on", Card::SDSC1, true},
    };
    for (auto &c : cases) {
        card.insert(c.type);
        card.crcOn = c.crcOn;                           // as CHSDtoUSB left it
        initAndRead(c.name, true);
    }
    {
        const char *name = "CHSDtoUSB, then a reset into the game";
        card.insert(Card::SDHC);
        initAndRead(name, true);
        card.state = Card::READY;                       // powered all along
        card.crcOn = true;                              // CHSDtoUSB's crcOn()
        initAndRead(name, true);
    }
    card.insert(Card::NONE);
    initAndRead("no card", false);
    printf(failures ? "FAILED (%d)\n" : "all %d cases passed\n", failures ? failures : 7);
    return failures ? 1 : 0;
}
