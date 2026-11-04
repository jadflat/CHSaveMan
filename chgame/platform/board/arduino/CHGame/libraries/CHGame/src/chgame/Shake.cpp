#include <CHGfx.h>
#include "Fx.h"
#include "RamFunc.h"

namespace fx {

// ---------------------------------------------------------------------------
// Shake
// ---------------------------------------------------------------------------
static uint8_t shakeT, shakeAmp;

void shake(uint8_t frames, uint8_t amp) { shakeT = frames; shakeAmp = amp; }
bool shaking() { return shakeT != 0; }
void shakeTick() { if (shakeT) shakeT--; }
void shakeStop() { shakeT = 0; }

// Rows y0..y1 moved dy rows and one byte (2 px) sideways, in one pass of
// word copies from SRAM (newlib's memmove is a byte loop in flash: ~10 ms a
// shaken frame). Walks away from the direction of travel so every source row
// is read before it is overwritten; rows the move uncovers shift in place.
CHGAME_RAMFUNC(shake) static void shiftRows(int y0, int y1, int dy, bool right) {
    const int W = GFX_FB_STRIDE / 4;
    for (int k = 0; k <= y1 - y0; k++) {
        int y = dy > 0 ? y1 - k : y0 + k, sy = y - dy;
        if (sy < y0 || sy > y1) sy = y;
        uint32_t *d = (uint32_t *)(gfx_fb + y * GFX_FB_STRIDE);
        const uint32_t *s = (const uint32_t *)(gfx_fb + sy * GFX_FB_STRIDE);
        if (right) {        // d[i] = s[i - 1], the first byte kept
            for (int j = W - 1; j > 0; j--) d[j] = (s[j] << 8) | (s[j - 1] >> 24);
            d[0] = (s[0] << 8) | (s[0] & 0xFF);
        } else {            // d[i] = s[i + 1], the last byte kept
            for (int j = 0; j < W - 1; j++) d[j] = (s[j] >> 8) | (s[j + 1] << 24);
            d[W - 1] = (s[W - 1] >> 8) | (s[W - 1] & 0xFF000000u);
        }
    }
}

void applyShake(int y0, int y1, int fill) {
    if (!shakeT) return;
    int a = (shakeAmp * shakeT + 9) / 10;
    if (a < 1) a = 1;
    int dy = (shakeT & 1) ? a : -a;
    if (fill >= 0) gfx_scroll(y0, y1 - y0 + 1, (shakeT & 2) ? 2 : -2, dy, fill);
    else shiftRows(y0, y1, dy, (shakeT & 2) != 0);               // 2 px sideways
}

}  // namespace fx
