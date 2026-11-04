/**
 * @file CHGfx.h
 * @brief CHGfx: a maximum-throughput ST7735 driver for the CHGame board
 *        (CH32X035G8U6 @ 48 MHz, QingKe V4C, 20 KB SRAM, 62 KB flash).
 *
 * **Why this exists.** Adafruit_GFX + Adafruit_ST7735 on this part is
 * slow for three separate reasons, and they compound:
 *
 *   1. Every drawPixel() sends CASET + RASET + RAMWR (11 bytes of command
 *      traffic) to paint 2 bytes of pixel. ~85% of the bus is overhead.
 *   2. The Arduino SPI class transfers one byte at a time in a polled loop
 *      (cores/.../libraries/SPI/src/utility/spi_com.c spins on TXE for
 *      every single byte). The CPU is the bottleneck, not the wire.
 *   3. There is no framebuffer, so overdraw goes straight out over SPI.
 *
 * **What this does instead.**
 *
 *   - Register-level SPI1 at HCLK/2 = 24 MHz. That is the CH32X035 hard
 *     ceiling: "Maximum clock frequency supports up to half of FHCLK"
 *     (Reference Manual ch.16.1), and HCLK cannot exceed 48 MHz because
 *     this part has no PLL, only the 48 MHz HSI RC (RM ch.3.3).
 *   - DMA1 channel 3 - the SPI1_TX request line (RM 9.2.3) - pushes whole
 *     chunks with zero CPU involvement.
 *   - 16-bit SPI data frames (DFF=1) so DMA does half as many bus cycles.
 *   - A 4 bpp (16-colour) framebuffer. 128*128 RGB565 is 32 KB and this
 *     chip has 20 KB, so a full-colour framebuffer is physically
 *     impossible. 4 bpp is 8 KB and leaves room to live.
 *   - A 256-entry uint32 lookup table expands one framebuffer byte
 *     (= 2 pixels) into one 32-bit store. Conversion costs ~3 cycles per
 *     pixel against a 32-cycle-per-pixel SPI budget, so the wire stays
 *     saturated. Free for the WIRE, not for the CPU: an async flush
 *     still spends its conversion time inside the DMA interrupt - see
 *     "What a flush costs the CPU" in @ref chgfx_present.
 *   - Optional 12 bpp (RGB444) output - ST7735 COLMOD 0x03. Three bytes
 *     per two pixels instead of four: 25% less traffic, and with a
 *     16-colour palette you lose nothing you were actually using.
 *   - Ping-pong chunk buffers driven by the DMA transfer-complete ISR, so
 *     gfx_flushAsync() returns immediately and game logic overlaps the
 *     ~11 ms it takes to shift a frame out.
 *   - The conversion inner loops are linked into .data so they execute
 *     from SRAM. Flash on this part runs at 3 wait states above 24 MHz
 *     (RM 20.3.1, FLASH_ACTLR LATENCY = 10b); SRAM runs at zero.
 *
 * **Ceiling.** 24 MHz SPI = 3.0 MB/s.
 *
 * | Output | Bytes a frame | Frame time | Frame rate |
 * |---|---:|---:|---:|
 * | 16 bpp, 128*128*2   | 32768 | 10.9 ms | 91 fps  |
 * | 12 bpp, 128*128*1.5 | 24576 |  8.2 ms | 122 fps |
 *
 * Nothing on this chip beats that: there is no parallel LCD interface,
 * no second SPI on these pins, and no PLL to go past 48 MHz HCLK.
 *
 * Everything is a plain C function (`gfx_*`); the CHGfx class and its one
 * instance, Gfx, are inline forwarders for the `Gfx.clear(0)` spelling.
 * The topics are listed under @ref lib_chgfx.
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "CHGfx_gfxfont.h"

#if !defined(CH32X035)
  #error "CHGfx targets the CH32X035. It drives SPI1 and DMA1 channel 3 directly and is not portable as-is. Select a CH32X035 board."
#endif

/**
 * @defgroup chgfx_config Configuration
 * @ingroup lib_chgfx
 * @brief Screen geometry, the panel's wiring, colour modes and SPI
 * dividers: the build options and the constants they define.
 *
 * The macros here can be overridden with `-D` build flags. They are
 * compiled into the library's own files, so they have to be build flags
 * (`--build-property build.extra_flags=-DGFX_CHUNK_ROWS=4` with
 * `arduino-cli`), not `#define`s in the sketch. Two more switches are
 * read only by the library's `.cpp` files:
 *
 * - `CHGFX_NO_SD_PARK`: do not drive the shared SD card's chip select
 *   (CHGFX_SDCS_PORT / CHGFX_SDCS_PIN) high at gfx_begin(). Define it if
 *   nothing else shares SPI1.
 * - `CHGFX_ISR_IN_SRAM`: run the DMA interrupt handler from SRAM. An
 *   async flush costs 4-9% less CPU (0.1-0.25 ms a full frame), for about
 *   330 bytes of SRAM.
 * @{
 */

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */
/* Override before including, or with a -D build flag. Tested at
 * 128x128; the framebuffer is W*H/2 bytes, so keep an eye on the 20 KB
 * SRAM budget if you raise these (160x128 = 10 KB, still fits). W must
 * be even and W*H/2 must be a multiple of 32. */
#ifndef GFX_W
/**
 * @brief Screen width in pixels (default 128).
 *
 * Override with a `-D` build flag. Must be a multiple of 8 (the row
 * operations work in 32-bit words), and W*H/2 a multiple of 32. The
 * framebuffer is W*H/2 bytes, so keep an eye on the 20 KB SRAM budget if
 * you raise it (160x128 = 10 KB, still fits).
 */
#define GFX_W            128
#endif
#ifndef GFX_H
/** @brief Screen height in pixels (default 128). Override with a `-D` build flag. */
#define GFX_H            128
#endif
/** @brief Bytes per framebuffer row: two pixels a byte, 64 at 128 wide. */
#define GFX_FB_STRIDE    (GFX_W / 2)
/** @brief Size of the framebuffer, gfx_fb, in bytes: 8192 at 128x128. */
#define GFX_FB_BYTES     (GFX_FB_STRIDE * GFX_H)

/* Rows converted per DMA chunk. Two chunk buffers are allocated, so
 * this directly costs GFX_W * GFX_CHUNK_ROWS * 4 bytes of SRAM. */
#ifndef GFX_CHUNK_ROWS
/**
 * @brief Framebuffer rows converted per DMA chunk (default 2).
 *
 * Two chunk buffers are allocated, so this directly costs
 * `GFX_W * GFX_CHUNK_ROWS * 4` bytes of SRAM. Override with
 * `-DGFX_CHUNK_ROWS=n`; it does not buy much CPU back either (an async
 * frame measured 2.49 / 2.33 / 2.29 ms of CPU at 2 / 4 / 8 rows, with
 * `CHGFX_ISR_IN_SRAM`).
 *
 * 2 rows = 512 B per buffer, 1 KB total. Measured against the obvious
 * 4 rows on real hardware: gfx_flush() went 11051 -> 11143 us, about
 * 0.8%, which is the same order as run-to-run variation. That buys back
 * a kilobyte, and on a part with 20 KB total a kilobyte is 5% of all
 * the memory there is - the right way round for this chip. Raise it if
 * you have RAM to spare and want the last percent.
 */
#define GFX_CHUNK_ROWS   2
#endif
/**
 * @brief Size of one DMA chunk buffer in bytes (worst case, 16 bpp).
 *
 * There are two; together they are the 1 KB gfx_chunkScratch().
 */
#define GFX_CHUNK_BYTES  (GFX_W * GFX_CHUNK_ROWS * 2)

/* ------------------------------------------------------------------ */
/* Wiring                                                              */
/* ------------------------------------------------------------------ */
/*
 * SCK and MOSI are fixed: they are SPI1's pins on this package (PA5 and
 * PA7), and SPI1 is the only peripheral with a DMA path to these lines.
 * The three control pins are yours to move - redefine any of these
 * before including CHGfx.h, or with -D build flags.
 *
 * Defaults are the CHGame board (see variant_CHGame_Rev0.h):
 *   PA4 = LCD_CS, PB0 = LCD_DC, PB12 = LCD_RST, PB11 = SD_CS
 * SD_CS is driven high at begin() because the microSD slot shares SPI1.
 */
#ifndef CHGFX_CS_PORT
/**
 * @brief GPIO port of the panel's chip select (default `GPIOA`, the
 * CHGame board's LCD_CS on PA4).
 *
 * The three control pins (CS, DC, RST) can be moved with `-D` build
 * flags; define the port and the pin together. SCK and MOSI are fixed:
 * they are SPI1's pins on this package (PA5 and PA7), and SPI1 is the
 * only peripheral with a DMA path to these lines.
 */
#define CHGFX_CS_PORT    GPIOA
/** @brief Pin number of the panel's chip select on CHGFX_CS_PORT (default 4). */
#define CHGFX_CS_PIN     4
#endif
#ifndef CHGFX_DC_PORT
/** @brief GPIO port of the panel's data/command line (default `GPIOB`, LCD_DC on PB0). */
#define CHGFX_DC_PORT    GPIOB
/** @brief Pin number of the panel's data/command line on CHGFX_DC_PORT (default 0). */
#define CHGFX_DC_PIN     0
#endif
#ifndef CHGFX_RST_PORT
/** @brief GPIO port of the panel's reset line (default `GPIOB`, LCD_RST on PB12). */
#define CHGFX_RST_PORT   GPIOB
/** @brief Pin number of the panel's reset line on CHGFX_RST_PORT (default 12). */
#define CHGFX_RST_PIN    12
#endif
/* Define CHGFX_NO_SD_PARK to skip driving a shared SD chip select high. */
#ifndef CHGFX_SDCS_PORT
/**
 * @brief GPIO port of the SD card's chip select, which shares SPI1
 * (default `GPIOB`, SD_CS on PB11).
 *
 * gfx_begin() drives it high so the card does not answer the traffic
 * meant for the panel. Define `CHGFX_NO_SD_PARK` to skip that.
 */
#define CHGFX_SDCS_PORT  GPIOB
/** @brief Pin number of the SD card's chip select on CHGFX_SDCS_PORT (default 11). */
#define CHGFX_SDCS_PIN   11
#endif

/* ------------------------------------------------------------------ */
/* Colour output modes                                                 */
/* ------------------------------------------------------------------ */
/**
 * @brief The panel's colour output modes, for gfx_begin() and
 * gfx_setColorMode().
 *
 * The framebuffer is always 4 bpp; this is what its palette colours are
 * expanded to on the wire, so the frame rate is just the byte count.
 * 12 bpp costs nothing visible behind a 16-colour palette and is a free
 * 25%, so the CHGame games use it.
 *
 * A note on 18 bpp. Each pixel is three bytes and each byte carries its
 * 6-bit component in bits 7:2, bits 1:0 don't-care (ST7735S DS 9.8.4).
 * 128*128*3 = 49152 B, so a frame costs 16.4 ms instead of 10.9 - you
 * are buying one extra bit of red and one of blue for a third of your
 * frame rate. Worth it for smooth gradients, pointless for sprite work.
 * Unlike 12 bpp it does not pass through the RGBSET conversion table:
 * the datasheet only defines 4k->262k and 65k->262k tables, so 18-bit
 * data reaches the frame memory directly.
 */
enum : uint8_t {
    GFX_16BPP = 0,   /**< ST7735 COLMOD 0x05, RGB565, 2 bytes/px   -> 90 fps */
    GFX_12BPP = 1,   /**< ST7735 COLMOD 0x03, RGB444, 1.5 bytes/px -> 119 fps */
    GFX_18BPP = 2    /**< ST7735 COLMOD 0x06, RGB666, 3 bytes/px   -> 61 fps */
};

/**
 * @brief SPI baud dividers (HCLK / n) for gfx_begin() and gfx_setSpiDiv().
 *
 * @note The ST7735S datasheet specifies tSCYCW(min) = 66 ns, i.e. 15 MHz.
 * 24 MHz is out of spec and only works on short flex. The Benchmark
 * example's SPI sweep exists to find out whether YOUR panel tolerates it;
 * GFX_DIV4 is comfortably in spec.
 */
enum : uint8_t { GFX_DIV2 = 2, GFX_DIV4 = 4, GFX_DIV8 = 8, GFX_DIV16 = 16 };
/** @var GFX_DIV2
 *  @brief HCLK/2 = 24 MHz: the fastest the chip can clock SPI, and the default. */
/** @var GFX_DIV4
 *  @brief HCLK/4 = 12 MHz: within the ST7735S datasheet's limit. */
/** @var GFX_DIV8
 *  @brief HCLK/8 = 6 MHz. */
/** @var GFX_DIV16
 *  @brief HCLK/16 = 3 MHz. */

/** @} */

/* ------------------------------------------------------------------ */
/* Framebuffer + palette                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief The framebuffer: GFX_FB_BYTES of 4 bpp pixels, two per byte.
 * @ingroup chgfx_draw
 *
 * Row y starts at `gfx_fb + y * GFX_FB_STRIDE`. Even x is in the LOW
 * nibble, odd x in the HIGH nibble - that ordering makes the expansion
 * LUT a single uint32 store with no shuffling on a little-endian core.
 * Each nibble is a palette index. Write it directly only for primitives
 * of your own, and only when no flush is reading it (after gfx_wait()).
 */
extern uint8_t  gfx_fb[GFX_FB_BYTES];
/**
 * @brief The 16 palette colours as set, in RGB565.
 * @ingroup chgfx_palette
 *
 * Read it freely; change it with gfx_setPalette() or
 * gfx_setPaletteEntry(), which stage the change for the next flush.
 * The fade (gfx_setFade()) is not applied here: gfx_paletteOut() gives
 * the colour the panel actually receives.
 */
extern uint16_t gfx_pal[16];

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_setup Setup
 * @ingroup lib_chgfx
 * @brief Starting the panel and changing its SPI clock, colour mode,
 * orientation and scan rate.
 * @{
 */

/* The two starts gfx_begin() picks between (see its documentation). */
void gfx__begin(uint8_t spiDiv, uint8_t colorMode);
void gfx__begin12(uint8_t spiDiv);
/**
 * @brief Starts the panel: pins, SPI1, DMA, a hardware reset and the
 * ST7735 initialisation, then a default 16-colour palette.
 *
 * Call it once in `setup()`. It takes about 0.7 s (the panel's reset
 * and wake-up delays). It parks the SD card's chip select high (unless
 * `CHGFX_NO_SD_PARK`), selects the colour mode and, in 12 bpp, programs
 * the panel's colour LUT (gfx_writeColorLut()). The framebuffer is not
 * touched and nothing is sent to the glass until the first flush.
 *
 * gfx_begin() picks one of two starts. A sketch that names GFX_12BPP and
 * never calls gfx_setColorMode() gets the 12 bpp-only start, and the 16
 * and 18 bpp converters and tables stay out of its image (0.2-0.4 KB of
 * flash, 176 B of it SRAM code). Any other mode, or one only known when
 * it runs, gets the general start, which keeps all three.
 *
 * @param spiDiv     SPI clock divider: GFX_DIV2 (24 MHz, the default),
 *                   GFX_DIV4, GFX_DIV8 or GFX_DIV16.
 * @param colorMode  GFX_16BPP (the default), GFX_12BPP or GFX_18BPP.
 *
 * @code
 * gfx_begin(GFX_DIV2, GFX_12BPP);   // what the CHGame games use
 * @endcode
 * @see CHGfx::begin()
 */
static inline void gfx_begin(uint8_t spiDiv = GFX_DIV2, uint8_t colorMode = GFX_16BPP) {
    if (colorMode == GFX_12BPP) gfx__begin12(spiDiv);
    else gfx__begin(spiDiv, colorMode);
}
/**
 * @brief Changes the SPI clock divider, and sets SPI1 up afresh for the panel.
 *
 * Waits for any flush in flight first, then programs all of SPI1 as
 * gfx_begin() does (its clock on, master, mode 0, MSB first, this divider).
 * That makes it the way to hand SPI1 back to CHGfx after anything else used
 * it, such as the Arduino @ref core_spi "SPI" class:
 * @code
 * gfx_wait();
 * SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
 * // ... talk to your own device ...
 * SPI.endTransaction();
 * gfx_setSpiDiv(gfx_spiDiv());     // the panel's set-up back before the next flush
 * gfx_flushAsync();
 * @endcode
 * @param div  GFX_DIV2, GFX_DIV4, GFX_DIV8 or GFX_DIV16 (32 also works);
 *             any other value runs at HCLK/2.
 */
void gfx_setSpiDiv(uint8_t div);
/**
 * @brief Switches the panel's colour output mode.
 *
 * Waits for any flush in flight, sends the panel its new COLMOD (and the
 * colour LUT when entering 12 bpp), and rebuilds the palette table for
 * the next flush. Does nothing if the mode is already set.
 * @param mode  GFX_16BPP, GFX_12BPP or GFX_18BPP.
 * @note Calling it anywhere in a sketch links the 16 and 18 bpp
 * converters, even if gfx_begin() was given GFX_12BPP.
 */
void gfx_setColorMode(uint8_t mode);
/**
 * @brief The current colour output mode.
 * @return GFX_16BPP, GFX_12BPP or GFX_18BPP.
 */
uint8_t gfx_colorMode(void);
/**
 * @brief The current SPI clock divider.
 * @return The divider last given to gfx_begin() or gfx_setSpiDiv().
 */
uint8_t gfx_spiDiv(void);
/**
 * @brief The current SPI clock.
 * @return The SPI clock in Hz: F_CPU divided by gfx_spiDiv(), 24000000 at
 *         GFX_DIV2.
 */
uint32_t gfx_spiHz(void);

/**
 * @brief Sets the panel's orientation (MADCTL) and the offsets of the
 * visible window in its memory.
 *
 * MADCTL / window offsets differ between 1.44" panel batches. Defaults
 * match Adafruit's INITR_144GREENTAB (madctl 0xC8, colstart 2, rowstart
 * 3), which is what an Adafruit_ST7735 setup is already using. Change
 * them if the image is offset or mirrored. Sends MADCTL at once, so call
 * it after gfx_wait().
 * @param madctl    ST7735 MADCTL (36h) value: rotation, mirroring, RGB/BGR.
 * @param colStart  First visible column of the panel's memory.
 * @param rowStart  First visible row of the panel's memory.
 */
void gfx_setPanelOffsets(uint8_t madctl, uint8_t colStart, uint8_t rowStart);
/**
 * @brief Turns the panel's colour inversion (INVON / INVOFF) on or off.
 *
 * Sends the command at once, so call it after gfx_wait().
 * @param on  true to invert every colour, false for normal.
 */
void gfx_setInverted(bool on);

/**
 * @brief Sets the panel's own refresh rate (ST7735 FRMCTR1, 0xB1).
 *
 * Lower values scan the glass faster, which cuts the latency between a
 * GRAM write and the pixel actually changing. The panel's rate is
 * 850 kHz / ((RTNA*2 + 40) * (lines + FPA + BPA)). Defaults to the fast
 * setting 0x05, 0x3A, 0x3A (Adafruit's is a lazy ~60 Hz 0x01, 0x2C,
 * 0x2D). Waits for any flush in flight first.
 * @param rtna  RTNA, the line period.
 * @param fpa   Front porch, in lines.
 * @param bpa   Back porch, in lines.
 */
void gfx_setPanelFrameRate(uint8_t rtna, uint8_t fpa, uint8_t bpa);

/** @} */

/**
 * @defgroup chgfx_palette Palette
 * @ingroup lib_chgfx
 * @brief The 16 colours the framebuffer's indices stand for, and a fade
 * applied on top of them.
 *
 * Palette changes are STAGED: they take effect when the next flush
 * starts, never part-way through one in flight, so these are safe to
 * call at any time - including between gfx_flushAsync() and gfx_wait().
 * Every frame is re-sent through the palette, so animating it (cycling a
 * slot, pulsing a highlight, a fade) costs a 256-entry table rebuild per
 * frame and no drawing at all.
 * @{
 */

/**
 * @brief Sets the first `count` palette entries.
 * @param rgb565  The colours, in RGB565 (see gfx_rgb()). Passing gfx_pal
 *                itself just re-stages the palette.
 * @param count   How many entries to set, from index 0; at most 16 are used.
 */
void gfx_setPalette(const uint16_t *rgb565, uint8_t count);
/**
 * @brief Sets one palette entry.
 * @param index   Palette index, 0..15 (only the low 4 bits are used).
 * @param rgb565  The colour, in RGB565.
 */
void gfx_setPaletteEntry(uint8_t index, uint16_t rgb565);

/**
 * @brief Fades every colour toward one colour (black by default).
 *
 * Applied while the expansion table is built, so gfx_pal[] keeps the
 * true colours and gfx_nearest() still matches against them. Staged like
 * the palette.
 * @param amount  0 = the palette as set, 255 = solid `rgb565`.
 * @param rgb565  The colour faded toward, in RGB565 (default black).
 */
void gfx_setFade(uint8_t amount, uint16_t rgb565 = 0x0000);
/**
 * @brief The current fade amount.
 * @return 0 (no fade) to 255 (every colour is the fade colour).
 */
uint8_t gfx_fade(void);

/**
 * @brief The colour a palette index actually reaches the panel as, fade
 * included.
 * @param index  Palette index, 0..15.
 * @return The colour in RGB565.
 */
uint16_t gfx_paletteOut(uint8_t index);
/**
 * @brief Packs an 8-bit-per-channel colour into RGB565.
 * @param r  Red, 0..255 (the top 5 bits are kept).
 * @param g  Green, 0..255 (the top 6 bits are kept).
 * @param b  Blue, 0..255 (the top 5 bits are kept).
 * @return The colour in RGB565, for gfx_setPalette() and the like.
 */
static inline uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** @} */

/* ------------------------------------------------------------------ */
/* Presenting the framebuffer                                          */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_present Presenting the framebuffer
 * @ingroup lib_chgfx
 * @brief Sending the framebuffer to the panel: whole or a rectangle,
 * blocking or by DMA in the background, and waiting for it.
 *
 * A flush converts the 4 bpp framebuffer through the palette into the
 * panel's format a chunk at a time and DMAs it out. The framebuffer is
 * free to draw into again after gfx_wait() (or, row by row, after
 * gfx_waitRow()). Every flush first waits for the one before it.
 *
 * **What a flush costs the CPU.** The wire time is fixed (11.1 ms a full
 * frame at 16 bpp, 8.4 ms at 12 bpp) and an async flush hands it to the
 * DMA. The pixel conversion still runs on the CPU, in the DMA interrupt,
 * one chunk at a time, and the DMA shares the bus: measured on the board,
 * an async full frame costs the main loop 2.5-3.2 ms of CPU (by -O2/-Os
 * and 12/16 bpp). At 60 fps that is a sixth of the CPU, taken from
 * whatever the main loop is doing - an AI search, for example. A partial
 * flush costs in proportion to its area. (Before 1.3, at the board's
 * default -Os, the converters were silently compiled into flash and the
 * same flush cost 5.1 ms.) `-DCHGFX_ISR_IN_SRAM` trims it by 4-9% for
 * ~330 bytes of SRAM.
 *
 * The cheapest frame is the one you do not draw: if nothing on screen
 * changed, flush the buffer you already have. A palette animation still
 * moves (the palette is applied during the flush), and the frame costs a
 * flush and no drawing.
 *
 * **Flush progress - "racing the beam".** The flush converts the
 * framebuffer top to bottom, and a row it has converted is never read
 * again, so it is free to draw into while the rest of the frame is still
 * going out. gfx_flushRow() is the first row the flush in flight may
 * still read; gfx_waitRow() waits for a band to be free. Useful when the
 * next frame is drawn top to bottom: wait for the top band, draw it, wait
 * for the next. A clip rectangle keeps the drawing honest:
 *
 * @code
 * gfx_waitRow(64);  gfx_setClip(0, 0, 128, 64);   drawTopHalf();
 * gfx_wait();       gfx_setClip(0, 64, 128, 64);  drawBottomHalf();
 * gfx_resetClip();  gfx_flushAsync();
 * @endcode
 *
 * **Partial updates.** The wire is the bottleneck, so sending a quarter
 * of the screen costs a quarter of the time - gfx_flushRect() is the
 * single biggest lever left once DMA is in place.
 * @{
 */

/**
 * @brief Sends the whole framebuffer to the panel and returns when it
 * has gone out.
 *
 * About 11.1 ms at 16 bpp, 8.4 ms at 12 bpp, at 24 MHz.
 * @see gfx_flushAsync(), gfx_flushRect()
 */
void gfx_flush(void);       /* convert + DMA the whole frame, blocking  */
/**
 * @brief Starts sending the whole framebuffer and returns at once; the
 * DMA interrupt does the rest.
 *
 * Do not draw into the framebuffer until gfx_wait() (or gfx_waitRow())
 * says the rows are free. The conversion costs the main loop 2.5-3.2 ms
 * of CPU a full frame (see above).
 */
void gfx_flushAsync(void);  /* same, but returns after the first chunk  */
/**
 * @brief Whether an async flush is still in flight.
 * @return true until the last byte of the flush has gone out.
 */
bool gfx_busy(void);
/**
 * @brief Waits until the flush in flight, if any, has finished.
 *
 * After it the framebuffer and gfx_chunkScratch() are free. Returns at
 * once when nothing is being sent.
 */
void gfx_wait(void);

/**
 * @brief The first framebuffer row the flush in flight may still read.
 * @return A row number; rows above it are free to draw into. GFX_H when
 *         no flush is in flight or the last chunk has been converted.
 */
int  gfx_flushRow(void);
/**
 * @brief Waits until rows 0..y-1 are free to draw into.
 * @param y  One past the last row needed. gfx_waitRow(GFX_H) is gfx_wait()
 *           as far as drawing goes.
 */
void gfx_waitRow(int y);

/** @} */

/* ------------------------------------------------------------------ */
/* Direct streaming - bypass the framebuffer entirely                  */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_stream Direct streaming
 * @ingroup lib_chgfx
 * @brief Full-colour procedural frames computed straight into the DMA's
 * buffers, with no framebuffer and no palette.
 *
 * The 4 bpp framebuffer exists because 128*128 RGB565 will not fit in
 * 20 KB. But a procedural effect - plasma, tunnel, rotozoomer, fire -
 * does not need a framebuffer at all: it can compute pixels straight
 * into the buffer the DMA is about to send. That gets you the panel's
 * FULL colour depth, 65536 or 262144 colours, with no framebuffer and
 * no palette.
 *
 * The callback (a gfx_streamFn) is called once per chunk while the
 * previous chunk is still on the wire, so the transfer never stalls as
 * long as you stay inside budget.
 *
 * **The budget, and it is the whole game:** 32 CPU cycles per pixel at
 * 16 bpp, 48 at 18 bpp. Stay under it and you render at full wire speed.
 * Go over and the frame rate degrades in proportion - nothing breaks, it
 * just slows. Put your inner loop in SRAM; flash costs 3 wait states here
 * and measured 2.2x slower on exactly this kind of loop.
 *
 * @code
 * void plasma(uint8_t *dst, int y0, int rows, void *user) {
 *     for (int r = 0; r < rows; r++)
 *         for (int x = 0; x < GFX_W; x++)
 *             dst = gfx_px565(dst, someColour(x, y0 + r));
 * }
 * gfx_stream(plasma, nullptr);
 * @endcode
 * @{
 */

/**
 * @brief A direct-streaming callback: fills `rows` rows starting at row
 * `y0` into `dst`, in the panel's native format.
 *
 * `dst` takes `rows * GFX_W * gfx_bytesPerPixel()` bytes: GFX_16BPP is
 * 2 bytes/px, RGB565 little-endian (write uint16_t, or gfx_px565());
 * GFX_18BPP is 3 bytes/px, each component in bits 7:2 (gfx_px666()).
 * `user` is the pointer given to gfx_stream().
 */
typedef void (*gfx_streamFn)(uint8_t *dst, int y0, int rows, void *user);
/**
 * @brief Draws one full-screen frame by calling `fn` for every chunk of
 * rows and sending the result straight to the panel.
 *
 * Blocking: waits for any flush in flight, then returns when the frame
 * has gone out. Requires GFX_16BPP or GFX_18BPP. In 12 bpp the packing
 * is 1.5 bytes per pixel with pixels straddling bytes, which is no use to
 * a per-pixel generator, so gfx_stream() switches to 16 bpp for you (and
 * the mode stays 16 bpp afterwards).
 * @param fn    The callback that fills each chunk.
 * @param user  Passed to every call of `fn` (may be nullptr).
 */
void gfx_stream(gfx_streamFn fn, void *user);

/**
 * @brief Bytes per pixel a stream callback writes in the current mode.
 * @return 2, or 3 for GFX_18BPP.
 */
uint8_t gfx_bytesPerPixel(void);

/* Pack helpers for stream callbacks. */
/**
 * @brief Writes one 16 bpp pixel for a stream callback.
 * @param p       Where to write it.
 * @param rgb565  The colour, in RGB565.
 * @return `p` advanced past the pixel (2 bytes).
 */
static inline uint8_t *gfx_px565(uint8_t *p, uint16_t rgb565) {
    *(uint16_t *)p = rgb565;
    return p + 2;
}
/**
 * @brief Writes one 18 bpp pixel for a stream callback.
 * @param p   Where to write it.
 * @param r6  Red, 0..63.
 * @param g6  Green, 0..63.
 * @param b6  Blue, 0..63.
 * @return `p` advanced past the pixel (3 bytes).
 */
static inline uint8_t *gfx_px666(uint8_t *p, uint8_t r6, uint8_t g6, uint8_t b6) {
    p[0] = (uint8_t)(r6 << 2);
    p[1] = (uint8_t)(g6 << 2);
    p[2] = (uint8_t)(b6 << 2);
    return p + 3;
}

/** @} */

/** @addtogroup chgfx_present
 *  @{
 */

/* Partial update. The wire is the bottleneck, so sending a quarter of
 * the screen costs a quarter of the time - this is the single biggest
 * lever left once DMA is in place. Declare what your frame actually
 * changed and a mostly-static scene runs several times faster. */
/**
 * @brief Sends one rectangle of the framebuffer to the panel and returns
 * when it has gone out.
 *
 * Declare what your frame actually changed and a mostly-static scene
 * runs several times faster: 64x64 goes out in 2.8 ms, 32x32 in 0.75 ms.
 * The rectangle is clipped to the screen (an empty one sends nothing).
 * x/w are rounded OUTWARD to a multiple of 2 (16 bpp) or 8 (12 bpp),
 * because the 4 bpp source has to start and end on a byte. Rounding out
 * is always safe - you just send a few more pixels than strictly needed.
 * @param x  Left edge, in pixels.
 * @param y  Top edge, in pixels.
 * @param w  Width, in pixels.
 * @param h  Height, in pixels.
 * @see gfx_flushRectAsync()
 */
void gfx_flushRect(int x, int y, int w, int h);
/**
 * @brief Starts sending one rectangle of the framebuffer and returns at
 * once, as gfx_flushAsync() does for the whole frame.
 *
 * Clipped and rounded as gfx_flushRect(). gfx_wait() before drawing
 * again.
 * @param x  Left edge, in pixels.
 * @param y  Top edge, in pixels.
 * @param w  Width, in pixels.
 * @param h  Height, in pixels.
 */
void gfx_flushRectAsync(int x, int y, int w, int h);

/** @} */

/* ------------------------------------------------------------------ */
/* Framebuffer drawing (all clipped, all operate on the 4 bpp buffer)  */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_draw Drawing
 * @ingroup lib_chgfx
 * @brief The clip rectangle, pixels, lines, rectangles, circles and
 * sprite blits, drawn into the 4 bpp framebuffer.
 *
 * Every colour is a palette index, 0..15 (only the low 4 bits are
 * used). Every call is clipped to the clip rectangle, and nothing reaches
 * the panel until a flush. Draw only after gfx_wait() (or gfx_waitRow()
 * for the rows you touch).
 *
 * **The clip rectangle.** Every call that paints pixels - fills, lines,
 * shapes, text, sprites, gfx_clear() - stays inside it. It is
 * intersected with the screen; an empty one (w or h <= 0) stops all
 * drawing. Defaults to the whole screen. gfx_getPixel(), gfx_scroll() and
 * the flush ignore it. Changing it costs nothing, so set it around a
 * panel or a HUD and put it back:
 *
 * @code
 * gfx_setClip(0, 10, 128, 118);   drawBoard();   gfx_resetClip();
 * @endcode
 * @{
 */

/**
 * @brief Sets the clip rectangle every drawing call stays inside.
 * @param x  Left edge, in pixels.
 * @param y  Top edge, in pixels.
 * @param w  Width; 0 or less stops all drawing.
 * @param h  Height; 0 or less stops all drawing.
 */
void gfx_setClip(int x, int y, int w, int h);
/** @brief Puts the clip rectangle back to the whole screen. */
void gfx_resetClip(void);
/**
 * @brief Reads the clip rectangle, as intersected with the screen.
 * @param[out] x  Left edge (may be nullptr).
 * @param[out] y  Top edge (may be nullptr).
 * @param[out] w  Width (may be nullptr).
 * @param[out] h  Height (may be nullptr).
 */
void gfx_getClip(int *x, int *y, int *w, int *h);

/**
 * @brief Fills the clip rectangle (normally the whole screen) with one
 * colour.
 *
 * The whole screen is one word store per 8 pixels, from SRAM.
 * @param c  Palette index.
 */
void gfx_clear(uint8_t c);
/**
 * @brief Sets one pixel.
 * @param x  Column.
 * @param y  Row.
 * @param c  Palette index.
 */
void gfx_pixel(int x, int y, uint8_t c);
/**
 * @brief Reads one pixel of the framebuffer (the clip is ignored).
 * @param x  Column.
 * @param y  Row.
 * @return The palette index there, or 0 off the screen.
 */
uint8_t gfx_getPixel(int x, int y);
/**
 * @brief Draws a horizontal line from (x, y), `w` pixels to the right.
 * @param x  Left end.
 * @param y  Row.
 * @param w  Length in pixels; 0 or less draws nothing.
 * @param c  Palette index.
 */
void gfx_hline(int x, int y, int w, uint8_t c);
/**
 * @brief Draws a vertical line from (x, y), `h` pixels down.
 * @param x  Column.
 * @param y  Top end.
 * @param h  Length in pixels; 0 or less draws nothing.
 * @param c  Palette index.
 */
void gfx_vline(int x, int y, int h, uint8_t c);
/**
 * @brief Fills a rectangle.
 * @param x  Left edge.
 * @param y  Top edge.
 * @param w  Width; 0 or less draws nothing.
 * @param h  Height; 0 or less draws nothing.
 * @param c  Palette index.
 */
void gfx_fillRect(int x, int y, int w, int h, uint8_t c);
/**
 * @brief Draws a rectangle's 1-pixel outline.
 * @param x  Left edge.
 * @param y  Top edge.
 * @param w  Width; 0 or less draws nothing.
 * @param h  Height; 0 or less draws nothing.
 * @param c  Palette index.
 */
void gfx_rect(int x, int y, int w, int h, uint8_t c);
/**
 * @brief Draws a line between two points, both ends included.
 * @param x0  Start column.
 * @param y0  Start row.
 * @param x1  End column.
 * @param y1  End row.
 * @param c   Palette index.
 */
void gfx_line(int x0, int y0, int x1, int y1, uint8_t c);
/**
 * @brief Draws a circle's 1-pixel outline, 2*r+1 pixels across.
 * @param cx  Centre column.
 * @param cy  Centre row.
 * @param r   Radius in pixels.
 * @param c   Palette index.
 */
void gfx_circle(int cx, int cy, int r, uint8_t c);
/**
 * @brief Draws a filled circle, 2*r+1 pixels across.
 * @param cx  Centre column.
 * @param cy  Centre row.
 * @param r   Radius in pixels.
 * @param c   Palette index.
 */
void gfx_fillCircle(int cx, int cy, int r, uint8_t c);

/**
 * @brief Copies a 4 bpp bitmap into the framebuffer, optionally with one
 * colour transparent.
 *
 * Source rows are packed the same way as the framebuffer (2 px/byte,
 * even x low nibble), each row padded to a whole byte. The opaque even-x
 * case runs at memcpy speed. For art with many runs of one colour, the
 * compact @ref chgfx_sprites "span sprites" are smaller.
 * @param spr          The bitmap, `(w + 1) / 2` bytes a row.
 * @param x            Left edge on screen.
 * @param y            Top edge on screen.
 * @param w            Width in pixels.
 * @param h            Height in pixels.
 * @param transparent  Colour index skipped, or -1 for an opaque copy.
 */
void gfx_blit(const uint8_t *spr, int x, int y, int w, int h, int transparent);

/** @} */

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_shapes Shapes
 * @ingroup lib_chgfx
 * @brief Rounded rectangles, ellipses, dithered fills and in-place
 * recolouring.
 *
 * Clipped like everything in @ref chgfx_draw. Nothing here costs a
 * sketch that does not call it: the linker drops what is unused.
 * @{
 */

/**
 * @brief Draws a rounded rectangle's 1-pixel outline.
 *
 * Corners are pixel-art arcs, not chamfers; r is clamped to half the
 * shorter side, and r = 0 is a plain rectangle.
 * @param x  Left edge.
 * @param y  Top edge.
 * @param w  Width; 0 or less draws nothing.
 * @param h  Height; 0 or less draws nothing.
 * @param r  Corner radius in pixels.
 * @param c  Palette index.
 */
void gfx_roundRect(int x, int y, int w, int h, int r, uint8_t c);
/**
 * @brief Fills a rounded rectangle (corners as gfx_roundRect()).
 * @param x  Left edge.
 * @param y  Top edge.
 * @param w  Width; 0 or less draws nothing.
 * @param h  Height; 0 or less draws nothing.
 * @param r  Corner radius in pixels.
 * @param c  Palette index.
 */
void gfx_fillRoundRect(int x, int y, int w, int h, int r, uint8_t c);

/**
 * @brief Draws an axis-aligned ellipse's 1-pixel outline, 2*rx+1 by
 * 2*ry+1 pixels, centred on (cx, cy).
 *
 * Integer only, no square roots.
 * @param cx  Centre column.
 * @param cy  Centre row.
 * @param rx  Horizontal radius in pixels.
 * @param ry  Vertical radius in pixels.
 * @param c   Palette index.
 */
void gfx_ellipse(int cx, int cy, int rx, int ry, uint8_t c);
/**
 * @brief Draws a filled axis-aligned ellipse, 2*rx+1 by 2*ry+1 pixels,
 * centred on (cx, cy).
 * @param cx  Centre column.
 * @param cy  Centre row.
 * @param rx  Horizontal radius in pixels.
 * @param ry  Vertical radius in pixels.
 * @param c   Palette index.
 */
void gfx_fillEllipse(int cx, int cy, int rx, int ry, uint8_t c);

/**
 * @brief Paints a 50% checkerboard of one colour over a rectangle,
 * leaving the pixels in between alone.
 *
 * Darkened backdrops behind menus, felt, shadows.
 * @param x      Left edge.
 * @param y      Top edge.
 * @param w      Width.
 * @param h      Height.
 * @param c      Palette index.
 * @param phase  0 paints pixels where x + y is even, 1 where it is odd.
 */
void gfx_dither(int x, int y, int w, int h, uint8_t c, uint8_t phase);

/**
 * @brief Recolours a rectangle in place through a 16-entry table:
 * pixel = remap[pixel].
 *
 * Dimming, tinting, a hit flash on what is already drawn. A rectangle of
 * 512 pixels or more builds a 256-entry table first and then costs one
 * lookup per two pixels.
 * @param x      Left edge.
 * @param y      Top edge.
 * @param w      Width.
 * @param h      Height.
 * @param remap  16 palette indices: the new colour for each old one.
 */
void gfx_remapRect(int x, int y, int w, int h, const uint8_t *remap);

/** @} */

/* ------------------------------------------------------------------ */
/* Span sprites (sprite4)                                              */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_sprites Span sprites
 * @ingroup lib_chgfx
 * @brief The compact run-length sprite format (sprite4): plain, scaled,
 * recoloured and rotated.
 *
 * The compact sprite format two CHGame titles arrived at independently.
 * Each row is a list of runs of one colour, so the art is small and a
 * run is a fill, not a pixel loop:
 *
 *     w, h, then per row:  n, then n bytes of  (len - 1) << 4 | colour
 *
 * Runs are 1..16 px. Colour 15 is transparent (a skip), and trailing
 * transparency is left out. `extras/sprite4.py` packs a PNG.
 *
 * Every pixel is drawn through remap[colour] (nullptr = as is), so ONE
 * image serves many looks: a team colour, a red damage flash, a white
 * hit flash, a solid silhouette for an outline. With 16 colours,
 * palette swapping is the natural way to get variety.
 *
 * @code
 * #include "slime.h"   // python extras/sprite4.py slime.png SLIME > slime.h
 * gfx_sprite4(SLIME, 40, 60);                // as drawn
 * gfx_sprite4(SLIME, 40, 60, redFlash);      // through a 16-entry remap
 * gfx_sprite4(SLIME, 40, 60, nullptr, 512);  // double size
 * @endcode
 * @{
 */

/**
 * @brief Draws a span sprite, optionally recoloured and scaled.
 *
 * Scaling is nearest neighbour, about the top-left corner. 1:1 has its
 * own fast path, so leave it at 256 unless you mean it.
 * @param spr    The sprite data (w, h, then the rows of runs).
 * @param x      Left edge on screen.
 * @param y      Top edge on screen.
 * @param remap  16 palette indices to draw each colour as, or nullptr
 *               (the default) for the colours as they are.
 * @param scale  Q8 scale: 256 = 1:1 (the default), 512 = double, 128 =
 *               half. 0 or less draws nothing.
 */
void gfx_sprite4(const uint8_t *spr, int x, int y,
                 const uint8_t *remap = nullptr, int scale = 256);
/**
 * @brief A span sprite's width.
 * @param spr  The sprite data.
 * @return Its width in pixels, at 1:1.
 */
static inline int gfx_sprite4W(const uint8_t *spr) { return spr[0]; }
/**
 * @brief A span sprite's height.
 * @param spr  The sprite data.
 * @return Its height in pixels, at 1:1.
 */
static inline int gfx_sprite4H(const uint8_t *spr) { return spr[1]; }

/**
 * @brief Draws a span sprite rotated and scaled about a pivot.
 *
 * The sprite's pixel (ax, ay) lands on screen at (px, py), turned by
 * `angle` and scaled by `scale`. The art is decoded into
 * gfx_chunkScratch(), so it must fit in 1 KB at 4 bpp (32x64, 45x45; a
 * larger sprite draws nothing) and this call waits for any flush in
 * flight first. Per pixel, so costlier than gfx_sprite4().
 * @param spr    The sprite data.
 * @param ax     Pivot column in the sprite.
 * @param ay     Pivot row in the sprite.
 * @param px     Screen column the pivot lands on.
 * @param py     Screen row the pivot lands on.
 * @param angle  Rotation, 256 = a full turn, clockwise on screen.
 * @param scale  Q8 scale, 256 = 1:1 (the default); below 8 counts as 8,
 *               0 or less draws nothing.
 * @param remap  16 palette indices to draw each colour as, or nullptr.
 */
void gfx_sprite4Rot(const uint8_t *spr, int ax, int ay, int px, int py,
                    uint8_t angle, int scale = 256, const uint8_t *remap = nullptr);

/** @} */

/* ------------------------------------------------------------------ */
/* Row operations                                                      */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_rows Row operations
 * @ingroup lib_chgfx
 * @brief Scrolling and stamping whole framebuffer rows at word speed.
 *
 * Word copies in SRAM. newlib-nano's memmove/memcpy are byte loops in
 * flash here: a screen shake built on memmove cost ~10 ms a frame.
 * @{
 */

/**
 * @brief Moves a band of rows by (dx, dy) pixels inside the band.
 *
 * Moves the band of rows y..y+h-1 by dx pixels right and dy rows down
 * (negative = left/up). Odd dx is fine (nibble shifts). It works on whole
 * rows and ignores the clip rectangle - it is a post-process: shake,
 * scrolling backgrounds.
 * @param y     First row of the band.
 * @param h     Rows in the band (clipped to the screen).
 * @param dx    Pixels to move right; negative moves left.
 * @param dy    Rows to move down; negative moves up.
 * @param fill  Palette index for the pixels uncovered by the move, or -1
 *              (the default) to leave them as they were.
 */
void gfx_scroll(int y, int h, int dx, int dy, int fill = -1);

/**
 * @brief Copies pixels [x0, x1) of a full-width row buffer into
 * framebuffer row y, clipped.
 *
 * Build a pattern row once, stamp it into many rows at word speed: tiled
 * floors, checkerboards, gradients.
 * @param y    Framebuffer row.
 * @param src  A row of GFX_FB_STRIDE bytes, packed like the framebuffer.
 * @param x0   First pixel copied.
 * @param x1   One past the last pixel copied.
 */
void gfx_copyRow(int y, const uint8_t *src, int x0, int x1);

/** @} */

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_text Text
 * @ingroup lib_chgfx
 * @brief Text in the built-in 5x7 font or any GFXfont: plain, scaled,
 * measured, and outlined/shadowed banners.
 *
 * The default font is the built-in 5x7, ASCII 32..126, 475 bytes, no
 * setup. gfx_setFont() swaps in a proportional GFXfont;
 * gfx_setFont(nullptr) puts the built-in one back.
 *
 * @code
 * #include <fonts/CHGfx_Sans12.h>
 * Gfx.setFont(&CHGfx_Sans12);
 * Gfx.print(4, 20, "Hello", WHITE);
 * @endcode
 *
 * GFXfont is byte-for-byte Adafruit's format (see CHGfx_gfxfont.h), so
 * anything from Adafruit_GFX's Fonts/ directory, or produced by either
 * font converter, works here as-is:
 *
 * @code
 * #include <Fonts/FreeSans9pt7b.h>
 * Gfx.setFont(&FreeSans9pt7b);
 * @endcode
 *
 * **The origin changes with the font.** Built-in font: y is the TOP of
 * the glyph box. Custom font: y is the BASELINE, with ascenders above it
 * and descenders below. Adafruit_GFX behaves exactly the same way, which
 * is why it is worth living with. gfx_fontBaseline() converts:
 *
 * @code
 * gfx_text(x, y + gfx_fontBaseline(), s, c);   // y = top, either font
 * @endcode
 *
 * A newline starts a new line at the original x; a carriage return is
 * ignored. Characters outside the font's range are drawn as '?', or
 * dropped if the font has no '?'.
 *
 * Bundled fonts live in src/fonts/ - see @ref chgfx_fonts for the list,
 * their flash cost, and how to convert your own with
 * `extras/fontconvert.py`.
 * @{
 */

/**
 * @brief Selects the font every text call uses from now on.
 *
 * Scans the glyphs once to find the font's ascent (for
 * gfx_fontBaseline()).
 * @param f  A GFXfont, or nullptr for the built-in 5x7.
 */
void gfx_setFont(const GFXfont *f);
/**
 * @brief The current font.
 * @return The GFXfont last given to gfx_setFont(), or nullptr for the
 *         built-in 5x7.
 */
const GFXfont *gfx_font(void);

/**
 * @brief Baseline-to-baseline line spacing of the current font, at scale 1.
 * @return Pixels: the font's yAdvance, or 8 for the built-in font.
 *         Multiply by your scale.
 */
int gfx_fontLineHeight(void);
/**
 * @brief The current font's ascent: how far its tallest ink reaches above
 * the baseline, at scale 1.
 * @return Pixels; 0 for the built-in font, since that one is already
 *         top-anchored. Add it to a top y to get the y to draw at.
 */
int gfx_fontBaseline(void);

/**
 * @brief Draws one character.
 * @param x   Left edge (pen position).
 * @param y   Top of the glyph box (built-in font) or baseline (GFXfont).
 * @param ch  The character.
 * @param c   Palette index.
 */
void gfx_char(int x, int y, char ch, uint8_t c);
/**
 * @brief Draws one character, scaled up by a whole number.
 * @param x      Left edge (pen position).
 * @param y      Top of the glyph box (built-in font) or baseline (GFXfont).
 * @param ch     The character.
 * @param c      Palette index.
 * @param scale  Integer scale, 1 or more (0 counts as 1).
 */
void gfx_charScaled(int x, int y, char ch, uint8_t c, uint8_t scale);
/**
 * @brief Draws a string in the current font.
 *
 * The built-in font advances 6 pixels a character.
 * @param x  Left edge of the first character.
 * @param y  Top of the glyph box (built-in font) or baseline (GFXfont).
 * @param s  Null-terminated string; a newline starts a new line at x.
 * @param c  Palette index.
 */
void gfx_text(int x, int y, const char *s, uint8_t c);
/**
 * @brief Draws a string in the current font, scaled up by a whole number.
 * @param x      Left edge of the first character.
 * @param y      Top of the glyph box (built-in font) or baseline (GFXfont).
 * @param s      Null-terminated string; a newline starts a new line at x.
 * @param c      Palette index.
 * @param scale  Integer scale, 1 or more (0 counts as 1).
 */
void gfx_textScaled(int x, int y, const char *s, uint8_t c, uint8_t scale);

/**
 * @brief Advance width of a string in the current font - the number to
 * use for centring and right-alignment.
 * @param s  Null-terminated string.
 * @return Width in pixels; for a multi-line string, the widest line.
 */
int gfx_textWidth(const char *s);
/**
 * @brief Advance width of a string in the current font at a scale.
 * @param s      Null-terminated string.
 * @param scale  Integer scale, 1 or more (0 counts as 1).
 * @return Width in pixels; for a multi-line string, the widest line.
 */
int gfx_textWidthScaled(const char *s, uint8_t scale);

/**
 * @brief Bounding box of a string drawn at (x, y).
 *
 * Same units and origin convention as gfx_text() - use it to frame or
 * erase text without guessing. With a custom font this is the tight ink
 * box; with the built-in font it is the 5x7 cell box, so a leading space
 * still counts. A string with no ink gives a 0x0 box at (x, y).
 * @param s       Null-terminated string.
 * @param x       Where the string would be drawn.
 * @param y       Where the string would be drawn (top or baseline, as
 *                gfx_text()).
 * @param scale   Integer scale, 1 or more (0 counts as 1).
 * @param[out] bx  Left edge of the box (may be nullptr).
 * @param[out] by  Top edge of the box (may be nullptr).
 * @param[out] bw  Width of the box (may be nullptr).
 * @param[out] bh  Height of the box (may be nullptr).
 */
void gfx_textBounds(const char *s, int x, int y, uint8_t scale,
                    int *bx, int *by, int *bw, int *bh);

/**
 * @brief Draws outlined, shadowed or gradient-filled text - titles and
 * banners.
 *
 * Same font, origin and scale rules as gfx_textScaled(). Drawing the
 * glyphs nine times over would cost ~9x a plain print. This renders the
 * text once into a 1 bpp mask in gfx_chunkScratch(), grows the outline
 * out of it with word-wide ORs and paints each row as spans, so a 3x
 * outlined, shadowed nine-letter banner costs about 2.9 ms. The mask -
 * the text's ink box plus a 1 px margin - must fit in 1 KB: text up to
 * 126x62 px, or 254x30. Waits for any flush in flight first.
 *
 * @param x        Left edge, as gfx_text().
 * @param y        Top (built-in font) or baseline (GFXfont), as gfx_text().
 * @param s        Null-terminated string.
 * @param scale    Integer scale, 1 or more.
 * @param fill     Palette index of the letters.
 * @param outline  Palette index of a 1 px ring around them (all 8
 *                 directions), or -1 (the default) for none.
 * @param shadow   Palette index of the outlined shape again, 1 px
 *                 down-right, underneath, or -1 (the default) for none.
 * @param ramp     Optional: a fill colour per pixel row of the text, top
 *                 to bottom (as many entries as the text is tall),
 *                 replacing `fill`; nullptr for none.
 * @param dy       Optional: a vertical offset per character, for wavy
 *                 text; nullptr for none.
 * @return false, having drawn nothing, if the text is too big for the
 *         mask; true otherwise (also when nothing was inside the clip).
 */
bool gfx_textFx(int x, int y, const char *s, uint8_t scale, uint8_t fill,
                int outline = -1, int shadow = -1,
                const uint8_t *ramp = nullptr, const int8_t *dy = nullptr);

/** @} */

/* ------------------------------------------------------------------ */
/* Direct-to-panel paths (bypass the framebuffer entirely)             */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_direct Direct to the panel
 * @ingroup lib_chgfx
 * @brief Talking to the ST7735 without the framebuffer: chip select,
 * commands, address windows, DMA fills and raw blits.
 *
 * These write to the panel at once, so call them only when no flush is
 * in flight (after gfx_wait()); the SD card shares the bus too. Raw
 * pixel data must already be in the panel's current format.
 *
 * @code
 * gfx_wait();
 * gfx_select();
 * gfx_setWindow(0, 0, 32, 32);
 * gfx_directBlit(pixels565, 32 * 32 * 2, true);
 * gfx_deselect();
 * @endcode
 * @{
 */

/**
 * @brief Asserts the panel's chip select.
 *
 * Assert once, stream a whole frame, deassert - that is the entire
 * performance thesis of this driver in two functions. The panel latches
 * nothing while CS is high.
 */
void gfx_select(void);
/** @brief Waits for the last byte to leave SPI1, then releases the panel's chip select. */
void gfx_deselect(void);

/**
 * @brief Sets the panel's address window and starts a memory write
 * (CASET, RASET, RAMWR).
 *
 * Pixel data sent after it fills the window left to right, top to
 * bottom. The panel offsets (gfx_setPanelOffsets()) are added; nothing
 * is clipped. Needs gfx_select() first.
 * @param x  Left edge, in screen pixels.
 * @param y  Top edge, in screen pixels.
 * @param w  Width in pixels, 1 or more.
 * @param h  Height in pixels, 1 or more.
 */
void gfx_setWindow(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
/**
 * @brief Sends one command byte (D/C low), then sets D/C back to data.
 * @param c  ST7735 command.
 */
void gfx_cmd(uint8_t c);
/**
 * @brief Sends one data byte (D/C high).
 * @param d  The byte.
 */
void gfx_data8(uint8_t d);

/**
 * @brief Programs the ST7735 RGBSET (2Dh) colour-depth conversion LUT for
 * the current mode.
 *
 * Done automatically for 12 bpp because the datasheet says the table
 * powers up "Random"; call gfx_setWriteColorLut(false) before
 * gfx_setColorMode() if you would rather trust the factory contents.
 */
void gfx_writeColorLut(void);
/**
 * @brief Chooses whether entering 12 bpp writes the colour LUT
 * (gfx_writeColorLut()).
 * @param on  true (the default) to write it, false to trust the panel's
 *            factory contents.
 */
void gfx_setWriteColorLut(bool on);

/**
 * @brief Fills a rectangle of the panel with one colour by DMA, bypassing
 * the framebuffer.
 *
 * DMA with memory-increment DISABLED: the DMA engine re-reads one
 * halfword N times. Zero RAM, zero CPU, full wire speed. Waits for any
 * flush in flight; handles chip select itself; not clipped.
 * @param x       Left edge, in screen pixels.
 * @param y       Top edge, in screen pixels.
 * @param w       Width in pixels.
 * @param h       Height in pixels.
 * @param rgb565  The colour, in RGB565.
 * @warning 16 bpp only - a 12 bpp solid colour has a 3-byte repeat that
 * does not fit in a single halfword.
 */
void gfx_directFillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565);

/**
 * @brief Sends a caller-owned buffer, already in panel format, by DMA and
 * waits for it.
 *
 * Needs gfx_select() and gfx_setWindow() first.
 * @param data      The pixel data.
 * @param bytes     Its length in bytes (even when `halfword`).
 * @param halfword  true to send 16-bit SPI frames (RGB565 as uint16_t,
 *                  16 bpp), false for bytes (12 and 18 bpp).
 */
void gfx_directBlit(const void *data, uint32_t bytes, bool halfword);

/**
 * @brief Sends bytes one at a time in a polled loop, for baseline
 * comparisons.
 * @param data   The bytes.
 * @param bytes  How many.
 */
void gfx_blockingWrite(const uint8_t *data, uint32_t bytes);

/** @} */

/* ------------------------------------------------------------------ */
/* Internals exposed for the benchmark                                 */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_internals Internals and scratch
 * @ingroup lib_chgfx
 * @brief The converters the Benchmark example measures, the flush's
 * chunk buffers as scratch memory, and the frame size.
 * @{
 */

/**
 * @brief Converts framebuffer rows into the panel's format, with the
 * converter in SRAM.
 *
 * _ram lives in SRAM, _flash lives in flash. Benchmarking both is how
 * you measure the 3-wait-state penalty.
 * @param dst   Where the panel-format bytes go.
 * @param row   First framebuffer row.
 * @param rows  How many rows.
 * @return Bytes written to `dst`.
 */
uint32_t gfx_convertRows_ram(uint8_t *dst, uint16_t row, uint16_t rows);
/**
 * @brief Converts framebuffer rows into the panel's format, with the
 * converter in flash (for comparison with gfx_convertRows_ram()).
 * @param dst   Where the panel-format bytes go.
 * @param row   First framebuffer row.
 * @param rows  How many rows.
 * @return Bytes written to `dst`.
 */
uint32_t gfx_convertRows_flash(uint8_t *dst, uint16_t row, uint16_t rows);
/**
 * @brief Converts a run of framebuffer bytes into the panel's format (the
 * SRAM converter every flush uses).
 * @param dst       Where the panel-format bytes go.
 * @param src       4 bpp source bytes, two pixels each.
 * @param srcBytes  How many source bytes.
 * @return Bytes written to `dst`.
 */
uint32_t gfx_convertSpan_ram(uint8_t *dst, const uint8_t *src, uint32_t srcBytes);

/**
 * @brief 1 KB of word-aligned scratch memory: the flush's chunk buffers.
 *
 * 2 * GFX_CHUNK_BYTES, idle from gfx_wait() until the next flush starts.
 * Handy for decoding, masks, building a flash page, an SD card block -
 * never across a flush. gfx_sprite4Rot() and gfx_textFx() use it, so it
 * does not survive them either.
 * @return The start of the scratch.
 */
uint8_t *gfx_chunkScratch(void);

/**
 * @brief Bytes the panel receives for a full frame in the current mode.
 * @return 32768 (16 bpp), 24576 (12 bpp) or 49152 (18 bpp) at 128x128.
 */
uint32_t gfx_frameBytes(void);

/** @} */

/* ------------------------------------------------------------------ */
/* Palette helper                                                      */
/* ------------------------------------------------------------------ */
/**
 * @brief Nearest palette index for a full RGB565 colour.
 * @ingroup chgfx_palette
 *
 * Linear search over 16 entries, by weighted distance, against the
 * palette as set (not as faded) - fine at setup time to build named
 * constants, far too slow to call per pixel.
 * @param rgb565  The colour, in RGB565.
 * @return The palette index, 0..15, closest to it.
 */
uint8_t gfx_nearest(uint16_t rgb565);

/* ------------------------------------------------------------------ */
/* Idiomatic Arduino wrapper                                           */
/* ------------------------------------------------------------------ */
/**
 * @defgroup chgfx_class The Gfx object
 * @ingroup lib_chgfx
 * @brief The CHGfx class and its one instance, Gfx: the same API with
 * Arduino-style names.
 * @{
 */

/**
 * @brief Arduino-style access to CHGfx: every method is a zero-cost
 * inline forwarder to a `gfx_*` function.
 *
 * There is exactly one panel, one SPI peripheral and one DMA channel, so
 * the driver state is inherently global - the class exists for the
 * familiar `Gfx.clear(0)` spelling, not to allow two instances. Use the
 * one instance, Gfx. Use whichever style you prefer; they are the same
 * code. Each method's full description is on the function it forwards to.
 *
 * @code
 * Gfx.begin(GFX_DIV2, GFX_12BPP);
 * Gfx.clear(0);
 * Gfx.print(4, 4, "Hello", 1);
 * Gfx.display();
 * @endcode
 */
class CHGfx {
public:
    /**
     * @brief Starts the panel.
     * @param spiDiv     SPI clock divider (default GFX_DIV2, 24 MHz).
     * @param colorMode  GFX_16BPP (the default), GFX_12BPP or GFX_18BPP.
     * @see gfx_begin()
     */
    void begin(uint8_t spiDiv = GFX_DIV2, uint8_t colorMode = GFX_16BPP) { gfx_begin(spiDiv, colorMode); }

    /** @name Configuration */
    /** @{ */
    /** @brief Changes the SPI clock divider. @param d GFX_DIV2..GFX_DIV16. @see gfx_setSpiDiv() */
    void setSpiDiv(uint8_t d)                        { gfx_setSpiDiv(d); }
    /** @brief Switches the colour output mode. @param m GFX_16BPP, GFX_12BPP or GFX_18BPP. @see gfx_setColorMode() */
    void setColorMode(uint8_t m)                     { gfx_setColorMode(m); }
    /** @brief The current colour output mode. @return GFX_16BPP, GFX_12BPP or GFX_18BPP. @see gfx_colorMode() */
    uint8_t colorMode() const                        { return gfx_colorMode(); }
    /** @brief The current SPI clock. @return Hz. @see gfx_spiHz() */
    uint32_t spiHz() const                           { return gfx_spiHz(); }
    /**
     * @brief Sets the panel's MADCTL and window offsets.
     * @param m   MADCTL value.
     * @param cs  First visible column.
     * @param rs  First visible row.
     * @see gfx_setPanelOffsets()
     */
    void setPanelOffsets(uint8_t m, uint8_t cs, uint8_t rs) { gfx_setPanelOffsets(m, cs, rs); }
    /** @brief Turns colour inversion on or off. @param on true to invert. @see gfx_setInverted() */
    void setInverted(bool on)                        { gfx_setInverted(on); }
    /**
     * @brief Sets the panel's own refresh rate (FRMCTR1).
     * @param r  RTNA.
     * @param f  Front porch.
     * @param b  Back porch.
     * @see gfx_setPanelFrameRate()
     */
    void setPanelFrameRate(uint8_t r, uint8_t f, uint8_t b) { gfx_setPanelFrameRate(r, f, b); }
    /** @} */

    /** @name Palette */
    /** @{ */
    /**
     * @brief Sets the first `n` palette entries.
     * @param p  RGB565 colours.
     * @param n  How many, at most 16.
     * @see gfx_setPalette()
     */
    void setPalette(const uint16_t *p, uint8_t n)    { gfx_setPalette(p, n); }
    /**
     * @brief Sets one palette entry.
     * @param i  Palette index, 0..15.
     * @param c  RGB565 colour.
     * @see gfx_setPaletteEntry()
     */
    void setPaletteEntry(uint8_t i, uint16_t c)      { gfx_setPaletteEntry(i, c); }
    /**
     * @brief Fades every colour toward one colour.
     * @param amount  0 = none, 255 = solid `rgb565`.
     * @param rgb565  The colour faded toward (default black).
     * @see gfx_setFade()
     */
    void setFade(uint8_t amount, uint16_t rgb565 = 0) { gfx_setFade(amount, rgb565); }
    /** @brief The current fade amount. @return 0..255. @see gfx_fade() */
    uint8_t fade() const                             { return gfx_fade(); }
    /**
     * @brief Nearest palette index for an RGB565 colour (slow: setup only).
     * @param rgb565  The colour.
     * @return Palette index, 0..15.
     * @see gfx_nearest()
     */
    uint8_t nearest(uint16_t rgb565) const           { return gfx_nearest(rgb565); }
    /**
     * @brief Packs an 8-bit-per-channel colour into RGB565.
     * @param r  Red, 0..255.
     * @param g  Green, 0..255.
     * @param b  Blue, 0..255.
     * @return RGB565.
     * @see gfx_rgb()
     */
    static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return gfx_rgb(r, g, b); }
    /** @} */

    /** @name Present */
    /** @{ */
    /** @brief Sends the whole framebuffer and waits for it. @see gfx_flush() */
    void display()                                   { gfx_flush(); }
    /**
     * @brief Draws one full-colour frame from a callback, bypassing the
     * framebuffer.
     * @param fn    Fills each chunk of rows.
     * @param user  Passed to `fn` (default nullptr).
     * @see gfx_stream()
     */
    void stream(gfx_streamFn fn, void *user = nullptr) { gfx_stream(fn, user); }
    /** @brief Starts sending the whole framebuffer by DMA and returns at once. @see gfx_flushAsync() */
    void displayAsync()                              { gfx_flushAsync(); }
    /**
     * @brief Sends one rectangle of the framebuffer and waits for it.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @see gfx_flushRect()
     */
    void displayRect(int x, int y, int w, int h)     { gfx_flushRect(x, y, w, h); }
    /**
     * @brief Starts sending one rectangle of the framebuffer and returns at once.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @see gfx_flushRectAsync()
     */
    void displayRectAsync(int x, int y, int w, int h){ gfx_flushRectAsync(x, y, w, h); }
    /** @brief Whether an async flush is in flight. @return true while sending. @see gfx_busy() */
    bool busy() const                                { return gfx_busy(); }
    /** @brief Waits for the flush in flight to finish. @see gfx_wait() */
    void wait()                                      { gfx_wait(); }
    /** @brief The first row the flush in flight may still read. @return A row, or GFX_H when idle. @see gfx_flushRow() */
    int  flushRow() const                            { return gfx_flushRow(); }
    /** @brief Waits until rows 0..y-1 are free to draw into. @param y One past the last row needed. @see gfx_waitRow() */
    void waitRow(int y)                              { gfx_waitRow(y); }
    /** @} */

    /** @name Clip */
    /** @{ */
    /**
     * @brief Sets the clip rectangle.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @see gfx_setClip()
     */
    void setClip(int x, int y, int w, int h)               { gfx_setClip(x, y, w, h); }
    /** @brief Puts the clip rectangle back to the whole screen. @see gfx_resetClip() */
    void resetClip()                                       { gfx_resetClip(); }
    /**
     * @brief Reads the clip rectangle.
     * @param[out] x  Left edge (may be nullptr).
     * @param[out] y  Top edge (may be nullptr).
     * @param[out] w  Width (may be nullptr).
     * @param[out] h  Height (may be nullptr).
     * @see gfx_getClip()
     */
    void getClip(int *x, int *y, int *w, int *h) const     { gfx_getClip(x, y, w, h); }
    /** @} */

    /** @name Draw */
    /** @{ */
    /** @brief Fills the clip rectangle with one colour. @param c Palette index. @see gfx_clear() */
    void clear(uint8_t c)                                  { gfx_clear(c); }
    /**
     * @brief Sets one pixel.
     * @param x  Column.
     * @param y  Row.
     * @param c  Palette index.
     * @see gfx_pixel()
     */
    void drawPixel(int x, int y, uint8_t c)                { gfx_pixel(x, y, c); }
    /**
     * @brief Reads one pixel.
     * @param x  Column.
     * @param y  Row.
     * @return Palette index, or 0 off the screen.
     * @see gfx_getPixel()
     */
    uint8_t getPixel(int x, int y) const                   { return gfx_getPixel(x, y); }
    /**
     * @brief Draws a horizontal line.
     * @param x  Left end.
     * @param y  Row.
     * @param w  Length.
     * @param c  Palette index.
     * @see gfx_hline()
     */
    void drawFastHLine(int x, int y, int w, uint8_t c)     { gfx_hline(x, y, w, c); }
    /**
     * @brief Draws a vertical line.
     * @param x  Column.
     * @param y  Top end.
     * @param h  Length.
     * @param c  Palette index.
     * @see gfx_vline()
     */
    void drawFastVLine(int x, int y, int h, uint8_t c)     { gfx_vline(x, y, h, c); }
    /**
     * @brief Fills a rectangle.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @param c  Palette index.
     * @see gfx_fillRect()
     */
    void fillRect(int x, int y, int w, int h, uint8_t c)   { gfx_fillRect(x, y, w, h, c); }
    /**
     * @brief Draws a rectangle's outline.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @param c  Palette index.
     * @see gfx_rect()
     */
    void drawRect(int x, int y, int w, int h, uint8_t c)   { gfx_rect(x, y, w, h, c); }
    /**
     * @brief Draws a line, both ends included.
     * @param x0  Start column.
     * @param y0  Start row.
     * @param x1  End column.
     * @param y1  End row.
     * @param c   Palette index.
     * @see gfx_line()
     */
    void drawLine(int x0, int y0, int x1, int y1, uint8_t c) { gfx_line(x0, y0, x1, y1, c); }
    /**
     * @brief Draws a circle's outline.
     * @param cx  Centre column.
     * @param cy  Centre row.
     * @param r   Radius.
     * @param c   Palette index.
     * @see gfx_circle()
     */
    void drawCircle(int cx, int cy, int r, uint8_t c)      { gfx_circle(cx, cy, r, c); }
    /**
     * @brief Draws a filled circle.
     * @param cx  Centre column.
     * @param cy  Centre row.
     * @param r   Radius.
     * @param c   Palette index.
     * @see gfx_fillCircle()
     */
    void fillCircle(int cx, int cy, int r, uint8_t c)      { gfx_fillCircle(cx, cy, r, c); }
    /**
     * @brief Copies a 4 bpp bitmap into the framebuffer.
     * @param s            The bitmap, packed like the framebuffer.
     * @param x            Left edge.
     * @param y            Top edge.
     * @param w            Width in pixels.
     * @param h            Height in pixels.
     * @param transparent  Colour index skipped, or -1 (the default) for opaque.
     * @see gfx_blit()
     */
    void drawSprite(const uint8_t *s, int x, int y, int w, int h, int transparent = -1)
                                                           { gfx_blit(s, x, y, w, h, transparent); }
    /**
     * @brief Draws a rounded rectangle's outline.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @param r  Corner radius.
     * @param c  Palette index.
     * @see gfx_roundRect()
     */
    void drawRoundRect(int x, int y, int w, int h, int r, uint8_t c) { gfx_roundRect(x, y, w, h, r, c); }
    /**
     * @brief Fills a rounded rectangle.
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     * @param r  Corner radius.
     * @param c  Palette index.
     * @see gfx_fillRoundRect()
     */
    void fillRoundRect(int x, int y, int w, int h, int r, uint8_t c) { gfx_fillRoundRect(x, y, w, h, r, c); }
    /**
     * @brief Draws an ellipse's outline.
     * @param cx  Centre column.
     * @param cy  Centre row.
     * @param rx  Horizontal radius.
     * @param ry  Vertical radius.
     * @param c   Palette index.
     * @see gfx_ellipse()
     */
    void drawEllipse(int cx, int cy, int rx, int ry, uint8_t c)      { gfx_ellipse(cx, cy, rx, ry, c); }
    /**
     * @brief Draws a filled ellipse.
     * @param cx  Centre column.
     * @param cy  Centre row.
     * @param rx  Horizontal radius.
     * @param ry  Vertical radius.
     * @param c   Palette index.
     * @see gfx_fillEllipse()
     */
    void fillEllipse(int cx, int cy, int rx, int ry, uint8_t c)      { gfx_fillEllipse(cx, cy, rx, ry, c); }
    /**
     * @brief Paints a 50% checkerboard of one colour over a rectangle.
     * @param x      Left edge.
     * @param y      Top edge.
     * @param w      Width.
     * @param h      Height.
     * @param c      Palette index.
     * @param phase  0 (the default) paints where x + y is even, 1 where odd.
     * @see gfx_dither()
     */
    void dither(int x, int y, int w, int h, uint8_t c, uint8_t phase = 0) { gfx_dither(x, y, w, h, c, phase); }
    /**
     * @brief Recolours a rectangle in place through a 16-entry table.
     * @param x      Left edge.
     * @param y      Top edge.
     * @param w      Width.
     * @param h      Height.
     * @param remap  16 palette indices.
     * @see gfx_remapRect()
     */
    void remapRect(int x, int y, int w, int h, const uint8_t *remap) { gfx_remapRect(x, y, w, h, remap); }
    /**
     * @brief Draws a span sprite.
     * @param s      The sprite data.
     * @param x      Left edge.
     * @param y      Top edge.
     * @param remap  16 palette indices, or nullptr (the default).
     * @param scale  Q8 scale, 256 = 1:1 (the default).
     * @see gfx_sprite4()
     */
    void drawSprite4(const uint8_t *s, int x, int y, const uint8_t *remap = nullptr, int scale = 256)
                                                           { gfx_sprite4(s, x, y, remap, scale); }
    /**
     * @brief Draws a span sprite rotated and scaled about a pivot.
     * @param s      The sprite data (at most 1 KB decoded).
     * @param ax     Pivot column in the sprite.
     * @param ay     Pivot row in the sprite.
     * @param px     Screen column of the pivot.
     * @param py     Screen row of the pivot.
     * @param angle  256 = a full turn, clockwise.
     * @param scale  Q8 scale, 256 = 1:1 (the default).
     * @param remap  16 palette indices, or nullptr (the default).
     * @see gfx_sprite4Rot()
     */
    void drawSprite4Rot(const uint8_t *s, int ax, int ay, int px, int py, uint8_t angle,
                        int scale = 256, const uint8_t *remap = nullptr)
                                                           { gfx_sprite4Rot(s, ax, ay, px, py, angle, scale, remap); }
    /**
     * @brief Moves a band of rows by (dx, dy) inside the band.
     * @param y     First row.
     * @param h     Rows.
     * @param dx    Pixels right (negative: left).
     * @param dy    Rows down (negative: up).
     * @param fill  Palette index for uncovered pixels, or -1 (the default) to keep them.
     * @see gfx_scroll()
     */
    void scroll(int y, int h, int dx, int dy, int fill = -1) { gfx_scroll(y, h, dx, dy, fill); }
    /**
     * @brief Copies pixels [x0, x1) of a row buffer into a framebuffer row.
     * @param y    Framebuffer row.
     * @param src  GFX_FB_STRIDE bytes, packed like the framebuffer.
     * @param x0   First pixel.
     * @param x1   One past the last pixel.
     * @see gfx_copyRow()
     */
    void copyRow(int y, const uint8_t *src, int x0, int x1)  { gfx_copyRow(y, src, x0, x1); }
    /**
     * @brief Draws one character.
     * @param x   Left edge.
     * @param y   Top (built-in font) or baseline (GFXfont).
     * @param ch  The character.
     * @param c   Palette index.
     * @see gfx_char()
     */
    void drawChar(int x, int y, char ch, uint8_t c)        { gfx_char(x, y, ch, c); }
    /**
     * @brief Draws one character, scaled.
     * @param x      Left edge.
     * @param y      Top (built-in font) or baseline (GFXfont).
     * @param ch     The character.
     * @param c      Palette index.
     * @param scale  Integer scale.
     * @see gfx_charScaled()
     */
    void drawChar(int x, int y, char ch, uint8_t c, uint8_t scale)
                                                           { gfx_charScaled(x, y, ch, c, scale); }
    /**
     * @brief Draws a string.
     * @param x  Left edge.
     * @param y  Top (built-in font) or baseline (GFXfont).
     * @param s  Null-terminated string.
     * @param c  Palette index.
     * @see gfx_text()
     */
    void print(int x, int y, const char *s, uint8_t c)     { gfx_text(x, y, s, c); }
    /**
     * @brief Draws a string, scaled.
     * @param x      Left edge.
     * @param y      Top (built-in font) or baseline (GFXfont).
     * @param s      Null-terminated string.
     * @param c      Palette index.
     * @param scale  Integer scale.
     * @see gfx_textScaled()
     */
    void print(int x, int y, const char *s, uint8_t c, uint8_t scale)
                                                           { gfx_textScaled(x, y, s, c, scale); }
    /**
     * @brief Draws outlined, shadowed or gradient-filled text.
     * @param x        Left edge.
     * @param y        Top (built-in font) or baseline (GFXfont).
     * @param s        Null-terminated string.
     * @param scale    Integer scale.
     * @param fill     Palette index of the letters.
     * @param outline  Palette index of the outline, or -1 (the default).
     * @param shadow   Palette index of the shadow, or -1 (the default).
     * @param ramp     A fill colour per pixel row, or nullptr (the default).
     * @param dy       A vertical offset per character, or nullptr (the default).
     * @return false if the text is too big for the 1 KB mask.
     * @see gfx_textFx()
     */
    bool printFx(int x, int y, const char *s, uint8_t scale, uint8_t fill, int outline = -1,
                 int shadow = -1, const uint8_t *ramp = nullptr, const int8_t *dy = nullptr)
                                                           { return gfx_textFx(x, y, s, scale, fill, outline, shadow, ramp, dy); }
    /** @} */

    /** @name Fonts
     * setFont(nullptr) returns to the built-in 5x7. Note that a custom
     * font takes y as the baseline - see @ref chgfx_text.
     */
    /** @{ */
    /** @brief Selects the font. @param f A GFXfont, or nullptr (the default) for the built-in 5x7. @see gfx_setFont() */
    void setFont(const GFXfont *f = nullptr)               { gfx_setFont(f); }
    /** @brief The current font. @return The GFXfont, or nullptr for the built-in one. @see gfx_font() */
    const GFXfont *font() const                            { return gfx_font(); }
    /** @brief Baseline-to-baseline line spacing. @return Pixels at scale 1. @see gfx_fontLineHeight() */
    int fontLineHeight() const                             { return gfx_fontLineHeight(); }
    /** @brief The font's ascent above the baseline. @return Pixels at scale 1; 0 for the built-in font. @see gfx_fontBaseline() */
    int fontBaseline() const                               { return gfx_fontBaseline(); }
    /** @brief Advance width of a string. @param s Null-terminated string. @return Pixels (the widest line). @see gfx_textWidth() */
    int textWidth(const char *s) const                     { return gfx_textWidth(s); }
    /**
     * @brief Advance width of a string at a scale.
     * @param s      Null-terminated string.
     * @param scale  Integer scale.
     * @return Pixels (the widest line).
     * @see gfx_textWidthScaled()
     */
    int textWidth(const char *s, uint8_t scale) const      { return gfx_textWidthScaled(s, scale); }
    /**
     * @brief Bounding box of a string drawn at (x, y).
     * @param s       Null-terminated string.
     * @param x       Where it would be drawn.
     * @param y       Where it would be drawn.
     * @param scale   Integer scale.
     * @param[out] bx  Left edge (may be nullptr).
     * @param[out] by  Top edge (may be nullptr).
     * @param[out] bw  Width (may be nullptr).
     * @param[out] bh  Height (may be nullptr).
     * @see gfx_textBounds()
     */
    void textBounds(const char *s, int x, int y, uint8_t scale,
                    int *bx, int *by, int *bw, int *bh) const
                                                           { gfx_textBounds(s, x, y, scale, bx, by, bw, bh); }
    /** @} */

    /** @name Direct to the panel */
    /** @{ */
    /**
     * @brief Fills a rectangle of the panel by DMA, bypassing the
     * framebuffer (16 bpp only).
     * @param x       Left edge.
     * @param y       Top edge.
     * @param w       Width.
     * @param h       Height.
     * @param rgb565  The colour.
     * @see gfx_directFillRect()
     */
    void fillRectDirect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t rgb565)
                                                           { gfx_directFillRect(x, y, w, h, rgb565); }
    /** @} */

    /** @name The raw framebuffer */
    /** @{ */
    /** @brief The framebuffer, for primitives of your own. @return gfx_fb. */
    uint8_t *buffer() const                                { return gfx_fb; }
    /** @brief Screen width. @return GFX_W. */
    static constexpr int width()  { return GFX_W; }
    /** @brief Screen height. @return GFX_H. */
    static constexpr int height() { return GFX_H; }
    /** @} */
};

/**
 * @brief The one CHGfx instance. Declared here, defined in CHGfx.cpp.
 *
 * `Gfx.clear(0)` is the same code as `gfx_clear(0)`.
 */
extern CHGfx Gfx;

/** @} */
