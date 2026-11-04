#include <CHGfx.h>
#include "Palette.h"

namespace pal {

// Authored on the RGB444 grid so 12 bpp output, fades and the simulator agree.
const uint16_t HOUSE[16] = {
    0x000,  // INK
    0xFFF,  // WHITE
    0x042,  // FELT_DK
    0x173,  // FELT
    0x4B5,  // FELT_LT
    0xBBC,  // SILVER
    0xE12,  // RED
    0x702,  // WINE
    0xFC2,  // GOLD
    0x741,  // WOOD
    0x26E,  // BLUE
    0x125,  // NAVY
    0xFB8,  // SKIN
    0x6EF,  // CYAN
    0xF0F,  // FX_A
    0xFC2,  // FX_B
};

const uint16_t FELTS[THEME_COUNT][3] = {
    {0x042, 0x173, 0x4B5},   // classic green
    {0x024, 0x149, 0x48D},   // blue
    {0x401, 0x812, 0xC44},   // red
    {0x203, 0x517, 0x95B},   // purple
};

static const uint16_t RAINBOW[12] = {
    0xF22, 0xF82, 0xFE2, 0x8F2, 0x2F4, 0x2FC, 0x2EF, 0x28F, 0x42F, 0xA2F, 0xF2E, 0xF28,
};
static const uint16_t SHIMMER[8]   = {0x6EF, 0x7EF, 0x9EF, 0xAFF, 0xBFF, 0xCFF, 0xEFF, 0xFFF};
static const uint16_t PULSE[8]     = {0xE12, 0xE32, 0xE52, 0xF72, 0xF82, 0xF92, 0xFB2, 0xFC2};
static const uint16_t FIRE_RAMP[8] = {0xF62, 0xF92, 0xFC2, 0xFE6, 0xFFC, 0xFE6, 0xFC2, 0xF92};

static uint16_t staged[16];
static const uint16_t (*felts)[3] = FELTS;
static uint8_t  feltCount = THEME_COUNT, themeIdx = 0, mode = CASINO;
static uint8_t  fadeLevel = 16, desat = 0;
static bool     dirty = true, cycling = true;
static uint32_t ticks = 0;
static uint8_t  flashIdx = 0xFF, flashFrames = 0;
static uint16_t flashColor = 0;

void init(const uint16_t *base) {
    for (uint8_t i = 0; i < 16; i++) staged[i] = base[i];
    dirty = true;
    commit();
}

void setThemes(const uint16_t (*f)[3], uint8_t count) { felts = f; feltCount = count; }

void setTheme(uint8_t t) {
    if (t >= feltCount) t = 0;
    themeIdx = t;
    for (uint8_t i = 0; i < 3; i++) staged[FELT_DK + i] = felts[t][i];
    dirty = true;
}

uint8_t theme()                  { return themeIdx; }
void setMode(uint8_t m)          { mode = m; }
void setFade(uint8_t level)      { if (level > 16) level = 16; if (level != fadeLevel) { fadeLevel = level; dirty = true; } }
uint8_t fade()                   { return fadeLevel; }
void setDesaturate(uint8_t a)    { if (a > 16) a = 16; if (a != desat) { desat = a; dirty = true; } }
void setFx(uint8_t i, uint16_t c){ staged[i] = c; dirty = true; }
void setCycling(bool on)         { cycling = on; }
uint16_t rgb444(uint8_t i)       { return staged[i]; }

void flash(uint8_t index, uint16_t c, uint8_t frames) {
    flashIdx = index; flashColor = c; flashFrames = frames; dirty = true;
}

// Triangle wave 0..15..0 over 32 frames.
static uint8_t tri(uint32_t t) { t &= 31; return (uint8_t)(t > 15 ? 31 - t : t); }

void tick() {
    ticks++;
    if (flashFrames && --flashFrames == 0) dirty = true;
    if (!cycling) return;
    uint16_t a, b;
    if (mode == TARGETS) {
        a = SHIMMER[tri(ticks * 2) >> 1];
        b = PULSE[tri(ticks * 2 + 16) >> 1];
    } else {
        a = mode == HOVER ? (uint16_t)(tri(ticks >> 1) * 0x111)
          : mode == FIRE  ? FIRE_RAMP[(ticks >> 1) & 7]
          : RAINBOW[(ticks / 3) % 12];
        // FX_B: triangle wave GOLD <-> WHITE over 32 frames.
        uint8_t t = tri(ticks);
        uint8_t g = (uint8_t)(12 + (t * 3) / 15), bl = (uint8_t)(2 + (t * 13) / 15);
        b = (uint16_t)(0xF00 | (g << 4) | bl);
    }
    // Only a tick that moves FX_A/FX_B marks the palette dirty.
    if (a != staged[FX_A] || b != staged[FX_B]) {
        staged[FX_A] = a; staged[FX_B] = b;
        dirty = true;
    }
}

void resetClock() { ticks = 0; }

void commit() {
    if (!dirty) return;
    dirty = false;
    uint16_t out[16];
    for (uint8_t i = 0; i < 16; i++) {
        uint16_t c = (i == flashIdx && flashFrames) ? flashColor : staged[i];
        int r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
        if (desat) {
            int y = (r * 5 + g * 9 + b * 2) >> 4;
            r += ((y - r) * desat) >> 4; g += ((y - g) * desat) >> 4; b += ((y - b) * desat) >> 4;
        }
        if (fadeLevel < 16) { r = (r * fadeLevel) >> 4; g = (g * fadeLevel) >> 4; b = (b * fadeLevel) >> 4; }
        out[i] = (uint16_t)((((r << 1) | (r >> 3)) << 11) | (((g << 2) | (g >> 2)) << 5) | ((b << 1) | (b >> 3)));
    }
    // gfx_pal is what was last set: only a real change costs a LUT rebuild.
    for (uint8_t i = 0; i < 16; i++)
        if (out[i] != gfx_pal[i]) { gfx_setPalette(out, 16); return; }
}

}  // namespace pal
