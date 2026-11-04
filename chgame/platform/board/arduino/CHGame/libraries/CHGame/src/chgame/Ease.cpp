// fx:: maths: easing, integer sine, randomness. Pure (no graphics), so host
// tests and game logic can link it on its own.
#include "Fx.h"

namespace fx {

// ---------------------------------------------------------------------------
// Curves: 17-point Q8 tables, linearly interpolated.
// ---------------------------------------------------------------------------
const int16_t EASE_CUBIC[17]  = {0, 45, 84, 119, 148, 173, 194, 210, 224, 235, 242, 248, 252, 254, 256, 256, 256};
const int16_t EASE_BACK[17]   = {0, 69, 126, 173, 209, 237, 257, 271, 278, 281, 281, 277, 272, 267, 261, 258, 256};
const int16_t EASE_INOUT[17]  = {0, 3, 11, 24, 40, 59, 81, 104, 128, 152, 175, 197, 216, 232, 245, 253, 256};
const int16_t EASE_BOUNCE[17] = {0, 8, 30, 68, 121, 189, 248, 215, 196, 193, 204, 231, 249, 240, 246, 253, 256};

int easeCurve(const int16_t *c, int t, int n) {
    if (n <= 0 || t >= n) return 256;
    if (t <= 0) return 0;
    int p = (t << 8) / n;                    // 0..255
    if (!c) return p;                        // LINEAR
    int i = p >> 4, f = p & 15;
    return c[i] + (((c[i + 1] - c[i]) * f) >> 4);
}

static const uint8_t SIN[65] = {
    0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 62, 68, 74, 80, 86, 92, 98, 104, 109, 115, 121, 126,
    132, 137, 142, 147, 152, 157, 162, 167, 172, 177, 181, 185, 190, 194, 198, 202, 206, 209,
    213, 216, 220, 223, 226, 229, 231, 234, 237, 239, 241, 243, 245, 247, 248, 250, 251, 252,
    253, 254, 255, 255, 255, 255, 255};

int isin(int a) {
    a &= 255;
    int q = a >> 6, i = a & 63;
    int v;
    switch (q) {
        case 0: v = SIN[i]; break;
        case 1: v = SIN[64 - i]; break;
        case 2: v = -SIN[i]; break;
        default: v = -SIN[64 - i]; break;
    }
    return v;
}

static uint32_t seed = 0x1234567u;
uint32_t rnd() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
void reseed() { seed = 0x1234567u; }
int rndRange(int lo, int hi) { return hi > lo ? lo + (int)(rnd() % (uint32_t)(hi - lo)) : lo; }

}  // namespace fx
