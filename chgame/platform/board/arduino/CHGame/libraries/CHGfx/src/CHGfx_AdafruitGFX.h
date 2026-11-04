/**
 * @file CHGfx_AdafruitGFX.h
 * @brief Drop CHGfx in underneath Adafruit_GFX: existing sketches keep
 * their drawing code and get the framebuffer and DMA transport.
 *
 * **Why you might want this.** Adafruit_GFX itself is not the slow part.
 * The slow part is Adafruit_ST7735 underneath it, pushing every pixel
 * down a polled SPI link with a fresh address window each time.
 * Adafruit_GFX's own feature set - print(), setTextSize(), the custom
 * GFXfont system, drawBitmap(), triangles, rounded rectangles - is
 * perfectly good.
 *
 * So: subclass Adafruit_GFX, point drawPixel() at the 4 bpp
 * framebuffer, override the span primitives with CHGfx's fast paths,
 * and present with display(). Existing sketches keep working and get
 * the full DMA transport underneath.
 *
 * @code
 * #include <Adafruit_GFX.h>      // see the note below - required
 * #include <CHGfx.h>
 * #include <CHGfx_AdafruitGFX.h>
 * CHGfx_GFX tft;
 *
 * void setup() {
 *     tft.begin();                       // same as Gfx.begin()
 *     tft.setPalette(myPalette, 16);
 *     tft.setTextColor(WHITE_IDX);
 *     tft.setCursor(4, 4);
 *     tft.print("hello");
 *     tft.display();                     // one DMA burst
 * }
 * @endcode
 *
 * **The one thing that changes.** Adafruit_GFX passes colours as
 * uint16_t. Here a colour is a PALETTE INDEX, 0..15 - there is no
 * 16-colour framebuffer that can hold an arbitrary RGB565. Passing
 * ST77XX_RED (0xF800) would be read as index 0, i.e. black, so map your
 * colours once at setup:
 *
 * @code
 * const uint8_t RED = Gfx.nearest(0xF800);
 * @endcode
 *
 * Set `CHGFX_GFX_AUTOMAP` if you would rather have colours mapped for you
 * at every call. It makes old code work verbatim but costs a 16-entry
 * search per drawing call, so it is off by default.
 *
 * **Fonts.** Adafruit_GFX's own setFont() keeps working here, and CHGfx's
 * bundled fonts are GFXfonts, so they go straight in:
 *
 * @code
 * #include <fonts/CHGfx_Sans12.h>
 * tft.setFont(&CHGfx_Sans12);
 * @endcode
 *
 * Note that tft.setFont() and Gfx.setFont() are SEPARATE state. The
 * bridge routes drawing through Adafruit_GFX, which does its own glyph
 * walking, so a font set on `Gfx` does not affect `tft` and vice versa.
 * Pick one text path per sketch.
 *
 * **Requires** the Adafruit GFX Library to be installed. This header is
 * not included by CHGfx.h - include it yourself only if you want the
 * bridge.
 *
 * You must also write `#include <Adafruit_GFX.h>` in your .ino, above
 * this include. That looks redundant, but the Arduino builder works out
 * which library include paths to add by scanning the SKETCH file only.
 * A library reached solely through another library's header never gets
 * its path added, and the include below would fail to resolve.
 */
#pragma once

#include "CHGfx.h"

#if !__has_include(<Adafruit_GFX.h>)
  #error "Adafruit_GFX.h not found. Install the Adafruit GFX Library, AND add \
'#include <Adafruit_GFX.h>' to your .ino ABOVE this include - the Arduino \
builder only resolves libraries that the sketch itself names."
#endif

#include <Adafruit_GFX.h>

/**
 * @defgroup chgfx_adafruit Adafruit_GFX bridge
 * @ingroup lib_chgfx
 * @brief CHGfx_GFX: an Adafruit_GFX whose drawing lands in CHGfx's
 * framebuffer, from `CHGfx_AdafruitGFX.h` (needs the Adafruit GFX
 * Library).
 *
 * See CHGfx_AdafruitGFX.h for the two things that change: colours are
 * palette indices, and the sketch must include `<Adafruit_GFX.h>` itself.
 * @{
 */

/**
 * @brief An Adafruit_GFX display that draws into CHGfx's 4 bpp
 * framebuffer and presents it by DMA.
 *
 * Adafruit_GFX's print(), fonts, bitmaps, triangles and the rest work as
 * usual; the pixel and span primitives go to CHGfx's fast paths. Every
 * `color` is a palette index, 0..15 (only the low 4 bits are used),
 * unless `CHGFX_GFX_AUTOMAP` is defined, in which case it is an RGB565
 * colour matched to the nearest palette entry at every call. Nothing
 * reaches the panel until display() or displayAsync().
 */
class CHGfx_GFX : public Adafruit_GFX {
public:
    /** @brief Makes a GFX_W x GFX_H display. Call begin() in `setup()`. */
    CHGfx_GFX() : Adafruit_GFX(GFX_W, GFX_H) { }

    /**
     * @brief Starts the panel, as gfx_begin().
     * @param spiDiv     SPI clock divider (default GFX_DIV2, 24 MHz).
     * @param colorMode  GFX_16BPP (the default), GFX_12BPP or GFX_18BPP.
     */
    void begin(uint8_t spiDiv = GFX_DIV2, uint8_t colorMode = GFX_16BPP) {
        gfx_begin(spiDiv, colorMode);
    }

    /* ---- the one method Adafruit_GFX actually requires ---- */
    /**
     * @brief Sets one pixel in the framebuffer (gfx_pixel()).
     * @param x      Column.
     * @param y      Row.
     * @param color  Palette index.
     */
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        gfx_pixel(x, y, idx(color));
    }

    /* ---- overrides that matter for speed --------------------------
     * Adafruit_GFX's defaults build every one of these out of
     * drawPixel(). Routing them at the framebuffer instead is where the
     * CPU-side win comes from: a filled span is word stores, 8 pixels
     * at a time, rather than a call and a clip test per pixel. */
    /**
     * @brief Draws a horizontal line (gfx_hline()).
     * @param x      Left end.
     * @param y      Row.
     * @param w      Length in pixels.
     * @param color  Palette index.
     */
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        gfx_hline(x, y, w, idx(color));
    }
    /**
     * @brief Draws a vertical line (gfx_vline()).
     * @param x      Column.
     * @param y      Top end.
     * @param h      Length in pixels.
     * @param color  Palette index.
     */
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        gfx_vline(x, y, h, idx(color));
    }
    /**
     * @brief Fills a rectangle (gfx_fillRect()).
     * @param x      Left edge.
     * @param y      Top edge.
     * @param w      Width.
     * @param h      Height.
     * @param color  Palette index.
     */
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        gfx_fillRect(x, y, w, h, idx(color));
    }
    /**
     * @brief Fills the screen (the clip rectangle) with one colour (gfx_clear()).
     * @param color  Palette index.
     */
    void fillScreen(uint16_t color) override {
        gfx_clear(idx(color));
    }
    /**
     * @brief Sets one pixel (gfx_pixel()); the same as drawPixel().
     * @param x      Column.
     * @param y      Row.
     * @param color  Palette index.
     */
    void writePixel(int16_t x, int16_t y, uint16_t color) override {
        gfx_pixel(x, y, idx(color));
    }
    /**
     * @brief Fills a rectangle (gfx_fillRect()); the same as fillRect().
     * @param x      Left edge.
     * @param y      Top edge.
     * @param w      Width.
     * @param h      Height.
     * @param color  Palette index.
     */
    void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        gfx_fillRect(x, y, w, h, idx(color));
    }
    /**
     * @brief Draws a horizontal line (gfx_hline()); the same as drawFastHLine().
     * @param x      Left end.
     * @param y      Row.
     * @param w      Length in pixels.
     * @param color  Palette index.
     */
    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        gfx_hline(x, y, w, idx(color));
    }
    /**
     * @brief Draws a vertical line (gfx_vline()); the same as drawFastVLine().
     * @param x      Column.
     * @param y      Top end.
     * @param h      Length in pixels.
     * @param color  Palette index.
     */
    void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        gfx_vline(x, y, h, idx(color));
    }
    /* Nothing to batch - we are writing to RAM, not to the panel. */
    /** @brief Does nothing: drawing goes to RAM, so there is nothing to batch. */
    void startWrite(void) override { }
    /** @brief Does nothing: drawing goes to RAM, so there is nothing to batch. */
    void endWrite(void)   override { }

    /* ---- presenting ----
     * Adafruit_ST7735 has no display() because it writes straight to the
     * glass. Here nothing reaches the panel until you ask. */
    /** @brief Sends the whole framebuffer to the panel and waits for it (gfx_flush()). */
    void display()                                    { gfx_flush(); }
    /** @brief Starts sending the whole framebuffer by DMA and returns at once (gfx_flushAsync()). */
    void displayAsync()                               { gfx_flushAsync(); }
    /**
     * @brief Sends one rectangle of the framebuffer and waits for it (gfx_flushRect()).
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     */
    void displayRect(int x, int y, int w, int h)      { gfx_flushRect(x, y, w, h); }
    /**
     * @brief Starts sending one rectangle of the framebuffer and returns at once
     * (gfx_flushRectAsync()).
     * @param x  Left edge.
     * @param y  Top edge.
     * @param w  Width.
     * @param h  Height.
     */
    void displayRectAsync(int x, int y, int w, int h) { gfx_flushRectAsync(x, y, w, h); }
    /** @brief Whether an async flush is in flight (gfx_busy()). @return true while sending. */
    bool busy() const                                 { return gfx_busy(); }
    /** @brief Waits for the flush in flight to finish (gfx_wait()); draw after it. */
    void wait()                                       { gfx_wait(); }

    /* ---- palette passthrough ---- */
    /**
     * @brief Sets the first `n` palette entries (gfx_setPalette()).
     * @param p  RGB565 colours.
     * @param n  How many, at most 16.
     */
    void setPalette(const uint16_t *p, uint8_t n) { gfx_setPalette(p, n); }
    /**
     * @brief Sets one palette entry (gfx_setPaletteEntry()).
     * @param i  Palette index, 0..15.
     * @param c  RGB565 colour.
     */
    void setPaletteEntry(uint8_t i, uint16_t c)   { gfx_setPaletteEntry(i, c); }
    /**
     * @brief Nearest palette index for an RGB565 colour (gfx_nearest()):
     * map Adafruit colour constants once at setup.
     * @param rgb565  The colour.
     * @return Palette index, 0..15.
     */
    uint8_t nearest(uint16_t rgb565) const        { return gfx_nearest(rgb565); }
    /**
     * @brief Switches the colour output mode (gfx_setColorMode()).
     * @param m  GFX_16BPP, GFX_12BPP or GFX_18BPP.
     */
    void setColorMode(uint8_t m)                  { gfx_setColorMode(m); }

private:
    static inline uint8_t idx(uint16_t color) {
#ifdef CHGFX_GFX_AUTOMAP
        /* Convenience mode: treat the argument as a real RGB565 and find
         * the closest palette slot. Correct for legacy code, but it is a
         * 16-entry search on every drawing call. */
        return gfx_nearest(color);
#else
        return (uint8_t)(color & 0x0F);
#endif
    }
};

/** @} */
