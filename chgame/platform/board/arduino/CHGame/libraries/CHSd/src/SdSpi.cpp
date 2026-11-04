// SdSpi.cpp - see SdSpi.h. From HypeRunner's src/sd/SdSpi.cpp (MIT, clean
// room: written from the SD Physical Layer Simplified Specification, chapter
// 7, and the CH32X035 reference manual), reduced to polled single-block
// reads. The simulator's stand-in is tools/chsim/host/sd_host.cpp.
//
// Speed: a block is about 0.5 ms of polled bytes at 12 MHz, and the card's
// own access time (CMD17 to the data token, 0.1-1 ms and more) is on top of
// that, so for read() a faster clock or DMA would save little. stream() is
// for the sketch that reads a whole file every frame (CHStlView): one
// command for a run of blocks, 24 MHz and DMA, the block before worked on
// while the next arrives. The games never call it, so the linker leaves it
// out of their images.
//
// Size notes (-Os, RV32, LTO): loop counters are word-sized (uint8_t ones
// cost a mask on every pass). The two extra flags below took 0-28 B off the
// games' images on 2026-10-01; on Fat.cpp they cost up to 88 B, so it keeps
// plain -Os (measure all three games before changing either).
#pragma GCC optimize("Os", "no-ipa-sra", "no-jump-tables", "no-guess-branch-probability")
#if !defined(CHSIM) && !defined(CHTEST)
#include <Arduino.h>
#include "SdSpi.h"

namespace sd {

// SPI1: master, software NSS held high, mode 0, 8-bit, MSB first.
static const uint32_t SPI_MASTER = (1u << 2) | (1u << 8) | (1u << 9);   // MSTR | SSI | SSM
static const uint32_t SPE = 1u << 6, RXNE = 1u << 0, BSY = 1u << 7;
static const uint8_t BR_IDENT = 7;              // 48 MHz / 256 = 187.5 kHz
static const uint8_t BR_RUN = 1;                // 12 MHz: polled bytes are slower than the wire anyway
// A block never read since power-up can take some cards 300-770 ms to
// deliver (measured by HypeRunner on a 16 GB card), hence the long token
// wait. The busy wait shares it: a read-only driver never leaves the card
// busy, and an empty slot reads 0xFF (not busy) at once. One constant lets
// the compiler fold it into wait() (16 B less than two).
static const uint32_t WAIT_US = 1000000, INIT_US = 1000000;

static uint8_t br;
static bool hc;                                 // block addressing (SDHC/SDXC)
static uint16_t lcdCtlr1;                       // CHGfx's SPI1 setup, put back afterwards

static uint8_t xfer(uint8_t b) {
    SPI1->DATAR = b;
    while (!(SPI1->STATR & RXNE)) {}
    return (uint8_t)SPI1->DATAR;
}

// BR and the frame size may change only while SPE is clear.
static void spiSet(uint32_t ctlr1) {
    while (SPI1->STATR & BSY) {}
    SPI1->CTLR1 = (uint16_t)(ctlr1 & ~SPE);
    SPI1->CTLR1 = (uint16_t)ctlr1;
}

static void claim() {
    GPIOA->BSHR = 1u << 4;                      // the panel deselected
    lcdCtlr1 = (uint16_t)SPI1->CTLR1;
    spiSet(SPI_MASTER | SPE | ((uint32_t)br << 3));
    (void)SPI1->DATAR;                          // the panel never reads: RXNE and OVR are set
    (void)SPI1->STATR;
    GPIOB->BCR = 1u << 11;                      // card selected
}

// The card drives DO until it sees a clock with CS high.
static void release() {
    GPIOB->BSHR = 1u << 11;
    xfer(0xFF);
    spiSet(lcdCtlr1);
}

// Clocks 0xFF until a start token arrives (tok: anything but 0xFF) or busy
// ends (!tok: 0xFF), or WAIT_US runs out. Returns the last byte.
static uint8_t wait(bool tok) {
    uint32_t t0 = micros();
    uint8_t b;
    do b = xfer(0xFF); while ((b == 0xFF) == tok && micros() - t0 <= WAIT_US);
    return b;
}

// 48-bit command frame: 01 + index, the argument MSB first, CRC7 and the end
// bit. Only CMD0 and CMD8 are checked by a card in SPI mode (CRC is left
// off): their CRCs are the two constants. R1 arrives within 8 bytes; bit 7
// set means nothing answered.
//
// One idle byte goes first: the SD spec's N_RC, at least 8 clocks between a
// response and the next command (Physical Layer Simplified Specification,
// SPI mode timing, 7.5). Many cards do without it; a strict one (a SanDisk
// 32 GB, "SK32G") is still finishing its last response, takes the command
// misaligned (CMD0 answered 01, then C1 7F, then every answer 6 bits off)
// and stops answering until it loses power. The bootloader's sd.c does the
// same. It costs one byte per command, not per block.
static uint8_t cmd(uint8_t c, uint32_t arg) {
    xfer(0xFF);
    xfer((uint8_t)(0x40 | c));
    for (int sh = 24; sh >= 0; sh -= 8) xfer((uint8_t)(arg >> sh));
    xfer(c == 8 ? 0x87 : 0x95);
    uint8_t r;
    uint32_t k = 9;
    do r = xfer(0xFF); while ((r & 0x80) && --k);
    return r;
}

static uint32_t rd32() {
    uint32_t r = 0;
    for (uint32_t i = 4; i; i--) r = (r << 8) | xfer(0xFF);
    return r;
}

// Identification at 187.5 kHz with CS low (spec figure 7-2).
static bool ident() {
    uint8_t r;
    uint32_t k = 20;
    wait(false);
    while (cmd(0, 0) != 0x01)                   // GO_IDLE_STATE: enter SPI mode
        if (!--k) return false;
    // CRC_ON_OFF off, the card's default, which the fixed CRC bytes rely on.
    // CHSDtoUSB turns checking on, the card stays powered across a reset (an
    // upload from CHSDtoUSB starts the game straight away), and CMD0 may not
    // turn it off again (the bootloader's sd.c does the same). Bit 0 is the
    // option; bits 31:1 are stuff bits, chosen so that the frame's CRC is
    // 0x95 like the rest: 12 B against 24 B for a third constant. A card that
    // minded them would leave CRC as it was, which is no worse than before.
    cmd(59, 0x9A);
    bool v2 = false;
    r = cmd(8, 0x1AA);                          // SEND_IF_COND 2.7-3.6 V, check pattern 0xAA
    if (!(r & 0x04)) {                          // not an illegal command: v2.00 or later
        if (r != 0x01 || (rd32() & 0xFFF) != 0x1AA) return false;
        v2 = true;
    }
    uint32_t t0 = micros();
    do {                                        // ACMD41 (HCS on v2) until idle clears
        r = cmd(55, 0);
        if (r <= 1) r = cmd(41, v2 ? 1ul << 30 : 0);
        if (r > 1 || micros() - t0 > INIT_US) return false;
    } while (r);
    hc = false;
    if (v2) {                                   // READ_OCR: CCS = block addressing
        if (cmd(58, 0)) return false;
        hc = (rd32() >> 30) & 1;
    }
    return hc || cmd(16, 512) == 0;             // SDSC: 512-byte blocks
}

bool init() {
    GPIOA->BSHR = 1u << 6;                      // MISO pulled up: an empty slot reads 0xFF
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFul << 24)) | (0x8ul << 24);
    br = BR_IDENT;
    claim();
    GPIOB->BSHR = 1u << 11;
    for (uint32_t k = 10; k; k--) xfer(0xFF);   // >= 74 clocks with CS high
    GPIOB->BCR = 1u << 11;
    bool ok = ident();
    release();
    br = BR_RUN;
    return ok;
}

bool read(uint32_t lba, uint8_t *dst) {
    claim();
    bool ok = wait(false) == 0xFF && cmd(17, hc ? lba : lba << 9) == 0 && wait(true) == 0xFE;
    if (ok) {
        for (uint32_t i = 0; i < 512; i++) dst[i] = xfer(0xFF);
        xfer(0xFF);                             // the CRC16, unused
        xfer(0xFF);
    }
    release();
    return ok;
}

// ---- stream(): CMD18 with DMA ---------------------------------------------
// Receiving takes clocks, and clocks come only from sending, so two DMA
// channels run together: channel 3 (SPI1 TX) sends 0xFF 512 times from one
// byte, channel 2 (SPI1 RX, the higher priority, so it is never overrun)
// stores what comes back. Neither raises an interrupt (CHGfx's handler is on
// channel 3; with TCIE clear it never runs for these); the RX channel's
// transfer-complete flag means every byte has been clocked and stored. The
// technique, and the 24 MHz the board's wiring takes, come from CHSDtoUSB's
// driver, which has moved gigabytes this way.
static const uint32_t DMA_EN = 1u << 0, DMA_M2P = 1u << 4, DMA_MINC = 1u << 7;
static const uint32_t DMA_CH23 = 0xFFu << 4, DMA_TC2 = 1u << 5, DMA_TE2 = 1u << 7;
static const uint8_t FILL = 0xFF;

static void dmaStart(uint8_t *dst) {
    DMA1_Channel2->CFGR = 0;
    DMA1_Channel3->CFGR = 0;
    DMA1->INTFCR = DMA_CH23;
    DMA1_Channel2->PADDR = (uint32_t)&SPI1->DATAR;
    DMA1_Channel2->MADDR = (uint32_t)dst;
    DMA1_Channel2->CNTR = 512;
    DMA1_Channel2->CFGR = (3u << 12) | DMA_MINC;            // very high priority
    DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
    DMA1_Channel3->MADDR = (uint32_t)&FILL;
    DMA1_Channel3->CNTR = 512;
    DMA1_Channel3->CFGR = (2u << 12) | DMA_M2P;             // high
    DMA1_Channel2->CFGR |= DMA_EN;                          // RX listening before TX clocks
    DMA1_Channel3->CFGR |= DMA_EN;
    SPI1->CTLR2 = 3;                                        // RXDMAEN | TXDMAEN
}

static bool dmaWait() {
    uint32_t t0 = micros();
    bool ok;
    while (!(ok = DMA1->INTFR & DMA_TC2) && !(DMA1->INTFR & DMA_TE2) && micros() - t0 < 2000) {}
    SPI1->CTLR2 = 0;
    DMA1_Channel2->CFGR = 0;
    DMA1_Channel3->CFGR = 0;
    DMA1->INTFCR = DMA_CH23;
    return ok;
}

bool stream(uint32_t lba, uint32_t n, uint8_t *buf0, uint8_t *buf1, BlockFn fn, void *ctx) {
    if (!n) return true;
    claim();
    spiSet(SPI_MASTER | SPE);                               // BR 0: 24 MHz
    bool ok = wait(false) == 0xFF && cmd(18, hc ? lba : lba << 9) == 0;
    const uint8_t *pending = nullptr;
    for (uint32_t k = 0; ok && k < n; k++) {
        uint8_t *cur = (k & 1) ? buf1 : buf0;
        if (wait(true) != 0xFE) { ok = false; break; }
        dmaStart(cur);
        if (pending) fn(pending, ctx);                      // while the block arrives
        if (!dmaWait()) { ok = false; break; }
        (void)SPI1->DATAR;
        xfer(0xFF);                                         // the CRC16, unused
        xfer(0xFF);
        pending = cur;
    }
    // CMD12 ends the stream (after a failure too: it is the one command a
    // card in a multi-block read listens to). A stuff byte comes before its
    // R1; any R1 will do (some cards flag "out of range" as they read
    // ahead). Its busy time runs alongside the last block's fn.
    xfer(0x4C);
    for (uint32_t i = 4; i; i--) xfer(0);
    xfer(0x61);
    xfer(0xFF);
    uint8_t r;
    uint32_t k = 9;
    do r = xfer(0xFF); while ((r & 0x80) && --k);
    release();
    if (ok && pending) fn(pending, ctx);
    return ok && !(r & 0x80);
}

}  // namespace sd
#endif
