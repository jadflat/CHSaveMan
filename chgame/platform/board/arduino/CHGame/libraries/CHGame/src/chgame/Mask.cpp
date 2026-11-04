#include <string.h>
#include <CHGfx.h>
#include "Mask.h"
#include "Draw.h"
#include "Palette.h"
#include "RamFunc.h"

Mask maskBegin(int w, int h) {
    Mask m;
    m.w = (uint8_t)w; m.h = (uint8_t)h;
    m.stride = (uint8_t)((w + 2 + 7) >> 3);
    m.bits = gfx_chunkScratch();
    memset(m.bits, 0, (uint32_t)m.stride * (uint32_t)(h + 2));   // newlib's: word stores
    return m;
}

int text35WidthScaled(const char *s, uint8_t scale) { return text35Width(s) * scale; }

// Each font row, scaled, is one bit pattern (3 * scale bits) ORed into
// `scale` mask rows: a few byte ORs a row rather than a call per pixel.
void maskText35(Mask &m, int x, int y, const char *s, uint8_t scale, const int8_t *dy) {
    uint32_t one = (1u << scale) - 1;
    for (int k = 0; s[k]; k++, x += 4 * scale) {
        if (s[k] == '~') { x -= 2 * scale; continue; }
        const uint8_t *g = glyph35(s[k]);
        if (!g) continue;
        int top = y + (dy ? dy[k] : 0) + 1, bx = x + 1;     // + the margin
        if (bx < 0) continue;
        for (int row = 0; row < 6; row++) {
            uint32_t pat = 0;
            for (int col = 0; col < 3; col++) pat = (pat << scale) | (((g[col] >> row) & 1) ? one : 0);
            if (!pat) continue;
            pat <<= 32 - 3 * scale - (bx & 7);
            for (int j = 0; j < scale; j++) {
                int r = top + row * scale + j;
                if ((unsigned)r >= (unsigned)(m.h + 2)) continue;
                uint8_t *p = m.bits + r * m.stride;
                for (int b = 0; b < 4 && (bx >> 3) + b < m.stride; b++) p[(bx >> 3) + b] |= (uint8_t)(pat >> (24 - 8 * b));
            }
        }
    }
}

// 1:1, mask row j + 1 is bitmap row j moved right by the 1 px margin, a
// byte at a time. The logos' padding bits are zero (tools/assets.py), so a
// carry out of a row's last byte is a real pixel, and then the mask row has
// a byte for it (w a multiple of 8). Scaled, pixel by pixel.
void maskBlit1(Mask &m, const uint8_t *bits, uint8_t w, uint8_t h, uint8_t scale) {
    uint8_t s = (uint8_t)((w + 7) >> 3);
    if (scale <= 1) {
        uint8_t *d = m.bits + m.stride;
        for (int j = 0; j < h; j++, d += m.stride) {
            uint8_t carry = 0;
            for (int b = 0; b < s; b++) {
                uint8_t v = *bits++;
                d[b] |= (uint8_t)(carry | v >> 1);
                carry = (uint8_t)(v << 7);
            }
            if (carry && s < m.stride) d[s] |= carry;
        }
        return;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!((bits[y * s + (x >> 3)] << (x & 7)) & 0x80)) continue;
            for (int dy = 0; dy < scale; dy++) {
                uint8_t *row = m.bits + (1 + y * scale + dy) * m.stride;     // + the margin
                for (int dx = 0; dx < scale; dx++) {
                    int px = 1 + x * scale + dx;
                    row[px >> 3] |= (uint8_t)(0x80 >> (px & 7));
                }
            }
        }
}

// Paint the set bits of one mask row (stride bytes, MSB-first) at screen
// row y, bit 0 at screen column x. A mask byte is eight pixels, four
// framebuffer bytes, so the bits go two at a time (at an odd x shifted one
// along first). From SRAM: this loop is most of a banner's cost.
CHGAME_RAMFUNC(maskruns) static void runs(const uint8_t *row, uint8_t stride, int x, int y, uint8_t c) {
    if ((unsigned)y >= GFX_H) return;
    uint8_t *fb = gfx_fb + y * GFX_FB_STRIDE;
    uint8_t cc = (uint8_t)(c | (c << 4));
    int odd = x & 1, px = x - odd;
    uint8_t prev = 0;
    for (int i = 0; i <= stride; i++, px += 8) {
        uint8_t cur = i < stride ? row[i] : 0;
        uint8_t b = odd ? (uint8_t)((prev << 7) | (cur >> 1)) : cur;
        prev = cur;
        for (int xx = px; b; xx += 2, b = (uint8_t)(b << 2)) {
            if ((unsigned)xx >= GFX_W) continue;
            uint8_t &q = fb[xx >> 1];
            switch (b & 0xC0) {
                case 0xC0: q = cc; break;
                case 0x80: q = (uint8_t)((q & 0xF0) | c); break;          // left pixel
                case 0x40: q = (uint8_t)((q & 0x0F) | (c << 4)); break;   // right pixel
            }
        }
    }
}

// Grow row r by one pixel in all 8 directions into out. From SRAM too: it
// runs for every row (the outline and the shadow share it).
CHGAME_RAMFUNC(maskdilate) static void dilateRow(const Mask &m, int r, uint8_t *out) {
    int rows = m.h + 2;
    for (int b = 0; b < m.stride; b++) {
        uint8_t v = m.bits[r * m.stride + b];
        if (r > 0) v |= m.bits[(r - 1) * m.stride + b];
        if (r + 1 < rows) v |= m.bits[(r + 1) * m.stride + b];
        out[b] = v;
    }
    uint8_t carryL = 0;
    uint8_t tmp[32];
    for (int b = 0; b < m.stride; b++) tmp[b] = out[b];
    for (int b = m.stride - 1; b >= 0; b--) {           // shift left (x-1)
        uint8_t nc = (uint8_t)(tmp[b] >> 7);
        out[b] |= (uint8_t)((tmp[b] << 1) | carryL);
        carryL = nc;
    }
    uint8_t carryR = 0;
    for (int b = 0; b < m.stride; b++) {                  // shift right (x+1)
        uint8_t nc = (uint8_t)(tmp[b] << 7);
        out[b] |= (uint8_t)((tmp[b] >> 1) | carryR);
        carryR = nc;
    }
}

void maskDraw(const Mask &m, int x, int y, uint8_t fill, int outline, int shadow, const uint8_t *ramp) {
    int rows = m.h + 2;
    int ox = x - 1, oy = y - 1;                          // undo the margin
    uint8_t d[32];
    // Each grown row is painted twice: as the shadow, a row down and a pixel
    // right, then as the outline. Screen row Y still gets its shadow before
    // its outline, as if all the shadow went down first. Without an outline
    // the shadow is the plain shape moved (1, 1).
    if (outline >= 0 || shadow >= 0)
        for (int r = 0; r < rows; r++) {
            const uint8_t *g = m.bits + r * m.stride;
            if (outline >= 0) { dilateRow(m, r, d); g = d; }
            if (shadow >= 0) runs(g, m.stride, ox + 1, oy + r + 1, (uint8_t)shadow);
            if (outline >= 0) runs(d, m.stride, ox, oy + r, (uint8_t)outline);
        }
    for (int r = 1; r < rows - 1; r++) runs(m.bits + r * m.stride, m.stride, ox, oy + r, ramp ? ramp[r - 1] : fill);
}

void maskPaint(const Mask &m, int x, int y, uint8_t c) {
    for (int r = 1; r <= m.h; r++) runs(m.bits + r * m.stride, m.stride, x - 1, y + r - 1, c);
}

// Mask row j, or the blank top margin outside it.
static const uint8_t *maskRow(const Mask &m, int j) {
    return (unsigned)j < (unsigned)(m.h + 2) ? m.bits + j * m.stride : m.bits;
}

// dst |= src moved k < 8 px right (n bytes, the last carry into dst[n]).
CHGAME_RAMFUNC(maskorright) static void orRight(const uint8_t *src, uint8_t *dst, int n, uint8_t k) {
    uint8_t carry = 0;
    for (int i = 0; i < n; i++) {
        dst[i] |= (uint8_t)((src[i] >> k) | carry);
        carry = (uint8_t)(src[i] << (8 - k));
    }
    dst[n] |= carry;
}

// Row by row. V(j), the union of the copies moved 0 .. depth + 1 px down
// and right, grown a pixel left and right, rolls through three rows; their
// OR, less the rest, is the outline and the shadow (INK). Copies 1 .. depth
// are the extrusion (`side`), copy 0 the face: the base, and over it a row's
// colour where the row's dither pattern (a nibble, repeated) has bits. A one-pixel
// gap between two face pixels on a row stays INK, so letters set close
// stay apart instead of running together down a stripe of extrusion. These
// rows are a pixel right of the mask's (room to grow on the left) and
// stride + 1 bytes long (the copies run past its right edge): depth <= 4.
void maskTitle(const Mask &m, int x, int y, uint8_t base, const uint8_t *ramp, uint8_t depth, uint8_t side) {
    const int W = m.stride + 1, ox = x - 2, oy = y - 1;
    uint8_t v[3][34], abf[102], *a = abf, *b = abf + 34, *f = abf + 68;
    uint8_t *v0 = v[0], *v1 = v[1], *v2 = v[2];
    memset(v, 0, sizeof v);
    for (int r = 0; r <= m.h + depth + 2; r++) {
        uint8_t *t = v0; v0 = v1; v1 = v2; v2 = t;      // V(r - 1), V(r); V(r + 1) goes in v2
        memset(abf, 0, sizeof abf);
        for (int k = 0; k <= depth + 1; k++) {
            orRight(maskRow(m, r + 1 - k), a, m.stride, (uint8_t)(k + 1));
            if (k && k <= depth) orRight(maskRow(m, r - k), b, m.stride, (uint8_t)(k + 1));
        }
        orRight(maskRow(m, r), f, m.stride, 1);          // the face (the margins outside 1 .. h)
        unsigned carry = 0, fc = 0;
        for (int i = 0; i < W; i++) {
            unsigned c = a[i], fi = f[i];
            v2[i] = (uint8_t)(c | (c >> 1) | carry | (c << 1) | (a[i + 1] >> 7));   // grown left and right
            carry = c << 7;
            unsigned gap = ~fi & ((fi >> 1) | fc) & ((fi << 1) | (f[i + 1] >> 7));
            fc = fi << 7;
            b[i] = (uint8_t)(b[i] & ~fi & ~gap);
            a[i] = (uint8_t)((v0[i] | v1[i] | v2[i]) & ~b[i] & ~fi);
        }
        runs(a, (uint8_t)W, ox, oy + r, INK);
        runs(b, (uint8_t)W, ox, oy + r, side);
        if (r < 1 || r > m.h) continue;
        uint8_t c = ramp[r - 1], pat = (uint8_t)((c & 15) * 0x11);   // its 4-pixel pattern, twice to the byte
        if (pat != 0xFF) runs(f, (uint8_t)W, ox, oy + r, base);
        if (!pat) continue;
        for (int i = 0; i < W; i++) f[i] &= pat;
        runs(f, (uint8_t)W, ox, oy + r, c >> 4);
    }
}

void titleArt(const uint8_t *bits, uint8_t w, uint8_t h, int x, int y, uint8_t base, const uint8_t *ramp,
              uint8_t depth, uint8_t side) {
    Mask m = maskBegin(w, h);
    maskBlit1(m, bits, w, h);
    maskTitle(m, x, y, base, ramp, depth, side);
}
