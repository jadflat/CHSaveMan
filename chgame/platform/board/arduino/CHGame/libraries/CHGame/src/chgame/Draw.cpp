#include <string.h>
#include <CHGfx_internal.h>         // gfx__clip: CHGfx's clip rectangle
#include "Draw.h"
#include "Fx.h"
#include "Palette.h"
#include "RamFunc.h"

static inline void plot(uint8_t *p, int x, uint8_t c) {
    if (x & 1) *p = (uint8_t)((*p & 0x0F) | (c << 4));
    else       *p = (uint8_t)((*p & 0xF0) | c);
}

// CHGfx's clip rectangle, [x0, x1) x [y0, y1), always inside the screen.
#define CLIP gfx__clip

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------
// Corner insets per row for radius 1..4 (pixel-art circles, not chamfers).
static const uint8_t INSET[4][4] = { {1}, {2, 1}, {3, 1, 1}, {4, 2, 1, 1} };

void fillRound(int x, int y, int w, int h, uint8_t r, uint8_t c) {
    if (r > 4) r = 4;
    if (r * 2 > h) r = (uint8_t)(h / 2);
    const uint8_t *in = INSET[r ? r - 1 : 0];
    for (int i = 0; i < r; i++) {
        gfx_hline(x + in[i], y + i, w - 2 * in[i], c);
        gfx_hline(x + in[i], y + h - 1 - i, w - 2 * in[i], c);
    }
    gfx_fillRect(x, y + r, w, h - 2 * r, c);
}

void roundRect(int x, int y, int w, int h, uint8_t r, uint8_t c) {
    if (r > 4) r = 4;
    if (r * 2 > h) r = (uint8_t)(h / 2);
    if (!r) { gfx_rect(x, y, w, h, c); return; }
    const uint8_t *in = INSET[r - 1];
    gfx_hline(x + in[0], y, w - 2 * in[0], c);
    gfx_hline(x + in[0], y + h - 1, w - 2 * in[0], c);
    for (uint8_t i = 1; i < r; i++) {
        int len = in[i - 1] - in[i]; if (len < 1) len = 1;
        gfx_hline(x + in[i], y + i, len, c);
        gfx_hline(x + w - in[i] - len, y + i, len, c);
        gfx_hline(x + in[i], y + h - 1 - i, len, c);
        gfx_hline(x + w - in[i] - len, y + h - 1 - i, len, c);
    }
    gfx_vline(x, y + r, h - 2 * r, c);
    gfx_vline(x + w - 1, y + r, h - 2 * r, c);
}

void panel(int x, int y, int w, int h, uint8_t r, uint8_t fill, uint8_t edge) {
    fillRound(x, y, w, h, r, fill);
    roundRect(x, y, w, h, r, edge);
}

const uint8_t DARKER[16] = {INK, SILVER, INK, FELT_DK, FELT, NAVY, WINE, INK,
                            WOOD, WINE, NAVY, INK, WOOD, BLUE, FX_A, WOOD};
const uint8_t LIGHTER[16] = {NAVY, WHITE, FELT, FELT_LT, WHITE, WHITE, SKIN, RED,
                             WHITE, GOLD, CYAN, BLUE, WHITE, WHITE, FX_A, WHITE};

// Every layer is the same rounded shape, so the shadow and the rims follow
// the corners.
void panelLit(int x, int y, int w, int h, uint8_t r, uint8_t fill, uint8_t edge) {
    uint8_t ri = r ? (uint8_t)(r - 1) : 0;
    fillRound(x + 1, y + 1, w, h, r, INK);
    fillRound(x, y, w, h, r, edge);
    fillRound(x + 1, y + 1, w - 2, h - 2, ri, DARKER[fill & 15]);
    fillRound(x + 1, y + 1, w - 2, h - 3, ri, LIGHTER[fill & 15]);
    fillRound(x + 1, y + 2, w - 2, h - 4, ri, fill);
}

void bevel(int x, int y, int w, int h, uint8_t light, uint8_t dark) {
    gfx_hline(x, y, w, light);
    gfx_vline(x, y, h, light);
    gfx_hline(x, y + h - 1, w, dark);
    gfx_vline(x + w - 1, y + 1, h - 1, dark);
}

void bevel(int x, int y, int w, int h, uint8_t c) { bevel(x, y, w, h, LIGHTER[c & 15], DARKER[c & 15]); }

void dither(int x, int y, int w, int h, uint8_t c, uint8_t phase) {
    const GfxClip &k = CLIP;
    if (x < k.x0) { w -= k.x0 - x; x = k.x0; }
    if (y < k.y0) { h -= k.y0 - y; y = k.y0; }
    if (x + w > k.x1) w = k.x1 - x;
    if (y + h > k.y1) h = k.y1 - y;
    if (w <= 0 || h <= 0) return;
    uint8_t cc = (uint8_t)(c | (c << 4));
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        uint8_t *row = gfx_fb + yy * GFX_FB_STRIDE;
        // Pixels where (px + yy + phase) is even get the colour.
        uint8_t m = ((yy + phase) & 1) ? 0xF0 : 0x0F;
        int i = x;
        if (i & 1) { if (m == 0xF0) row[i >> 1] = (uint8_t)((row[i >> 1] & 0x0F) | (c << 4)); i++; }
        // Whole bytes [p, e): a word (8 px) at a time from each aligned one.
        uint8_t *p = row + (i >> 1), *e = row + ((x + w) >> 1);
        uint32_t m32 = m * 0x01010101u, c32 = (cc & m) * 0x01010101u;
        while (p < e) {
            if (!((uintptr_t)p & 3))
                for (; p + 4 <= e; p += 4) *(uint32_t *)p = (*(uint32_t *)p & ~m32) | c32;
            if (p < e) { *p = (uint8_t)((*p & ~m) | (cc & m)); p++; }
        }
        if (((x + w) & 1) && m == 0x0F) *e = (uint8_t)((*e & 0xF0) | c);
    }
}

// The checker on every other row (the even ones): 25% of the pixels, an
// even grid of dots, fixed to the screen like dither()'s.
void spotlight(int cx, int cy, int r, uint8_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy, w = 0;
        if (y & 1) continue;
        while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
        dither(cx - w, y, 2 * w + 1, 1, c, 0);
    }
}

void dropShadow(int x, int y, int w, int h) {
    dither(x + 2, y + h, w, 2, INK, 0);
    dither(x + w, y + 2, 2, h - 2, INK, 0);
}

void remapRect(int x, int y, int w, int h, const uint8_t *m) {
    const GfxClip &k = CLIP;
    if (x < k.x0) { w -= k.x0 - x; x = k.x0; }
    if (y < k.y0) { h -= k.y0 - y; y = k.y0; }
    if (x + w > k.x1) w = k.x1 - x;
    if (y + h > k.y1) h = k.y1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint8_t *row = gfx_fb + (y + j) * GFX_FB_STRIDE;
        int i = x;
        if (i & 1) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)((b & 0x0F) | (m[b >> 4] << 4)); i++; }
        for (; i + 1 < x + w; i += 2) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)(m[b & 15] | (m[b >> 4] << 4)); }
        if (i < x + w) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)((b & 0xF0) | m[b & 15]); }
    }
}

// Scanline fill: each edge is walked once in 16.16 fixed point, keeping the
// leftmost and rightmost pixel centre each row covers.
void fillConvex(const int16_t *xy, uint8_t n, uint8_t c, int dither) {
    int ymin = 0x7FFF, ymax = -0x7FFF;
    for (uint8_t i = 0; i < n; i++) {
        int y = xy[2 * i + 1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    int r0 = (ymin - 8 + 15) >> 4, r1 = (ymax - 8) >> 4;     // rows whose centre is inside
    if (r0 < 0) r0 = 0;
    if (r1 > GFX_H - 1) r1 = GFX_H - 1;
    if (r0 > r1) return;
    int16_t xl[GFX_H], xr[GFX_H];
    for (int y = r0; y <= r1; y++) { xl[y] = 0x7FFF; xr[y] = -0x7FFF; }
    for (uint8_t i = 0; i < n; i++) {
        int x0 = xy[2 * i], y0 = xy[2 * i + 1];
        int x1 = xy[2 * ((i + 1) % n)], y1 = xy[2 * ((i + 1) % n) + 1];
        if (y0 > y1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        int a = (y0 - 8 + 15) >> 4, b = (y1 - 8 - 1) >> 4;   // centres in [y0, y1)
        if (y1 == y0) continue;
        int32_t dx = ((int32_t)(x1 - x0) << 16) / (y1 - y0);
        if (a < r0) a = r0;
        if (b > r1) b = r1;
        for (int y = a; y <= b; y++) {
            int32_t x = ((int32_t)x0 << 16) + dx * ((y << 4) + 8 - y0);
            int16_t xq = (int16_t)(x >> 16);
            if (xq < xl[y]) xl[y] = xq;
            if (xq > xr[y]) xr[y] = xq;
        }
    }
    for (int y = r0; y <= r1; y++) {
        if (xl[y] > xr[y]) continue;
        int a = (xl[y] - 8 + 15) >> 4, b = (xr[y] - 8) >> 4;
        if (b < a) continue;
        if (dither >= 0) ::dither(a, y, b - a + 1, 1, c, (uint8_t)dither);
        else gfx_hline(a, y, b - a + 1, c);
    }
}

// ---------------------------------------------------------------------------
// Span sprites
// ---------------------------------------------------------------------------
const uint8_t RM_ID[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// A run of one colour within a row, clipped to [x0, x1): odd nibble, whole
// bytes, odd nibble. pair is the colour in both nibbles.
static inline __attribute__((always_inline)) void pairRun(uint8_t *row, int a, int len, uint8_t pair,
                                                          int x0, int x1) {
    if (a < x0) { len -= x0 - a; a = x0; }
    if (a + len > x1) len = x1 - a;
    if (len <= 0) return;
    uint8_t *p = row + (a >> 1);
    if (a & 1) { *p = (uint8_t)((*p & 0x0F) | (pair & 0xF0)); p++; len--; }
    uint8_t *e = p + (len >> 1);
    while (p < e) *p++ = pair;
    if (len & 1) *p = (uint8_t)((*p & 0xF0) | (pair & 0x0F));
}

CHGAME_RAMFUNC(sprite4) void sprite4(const uint8_t *d, int x, int y, const uint8_t *remap, int scale,
                                     uint8_t flip) {
    uint8_t w = d[0], h = d[1];
    d += 2;
    const GfxClip &k = CLIP;
    uint8_t pair[15];                       // remapped colours, doubled (remap < 16)
    for (int i = 0; i < 15; i++) pair[i] = (uint8_t)((remap ? remap[i] : i) * 0x11);
    bool mirror = (flip & SPR_FLIP_H) && scale == 256;
    for (int jj = 0; jj < h; jj++) {
        int j = (flip & SPR_FLIP_V) ? h - 1 - jj : jj;     // upside down: source row jj to row h-1-jj
        uint8_t n = *d++;
        const uint8_t *runs = d;
        d += n;
        if (scale == 256) {
            // 1:1 (nearly always): a running x; mirrored, it runs leftwards
            // from the right edge.
            if (y + j < k.y0 || y + j >= k.y1) continue;
            uint8_t *row = gfx_fb + (y + j) * GFX_FB_STRIDE;
            int q = mirror ? x + w : x;
            for (uint8_t i = 0; i < n; i++) {
                uint8_t b = runs[i];
                int len = (b >> 4) + 1;
                if (mirror) q -= len;
                if ((b & 15) != 15) pairRun(row, q, len, pair[b & 15], k.x0, k.x1);
                if (!mirror) q += len;
            }
            continue;
        }
        // Scaled: source row j covers screen rows [j * scale,
        // (j + 1) * scale) >> 8, and a run [px, px + len) the columns scaled
        // the same way (trailing transparency is implicit).
        for (int yy = y + ((j * scale) >> 8); yy < y + (((j + 1) * scale) >> 8); yy++) {
            if (yy < k.y0 || yy >= k.y1) continue;
            uint8_t *row = gfx_fb + yy * GFX_FB_STRIDE;
            int px = 0;
            for (uint8_t i = 0; i < n; i++) {
                uint8_t b = runs[i];
                int len = (b >> 4) + 1;
                if ((b & 15) != 15) {
                    int a = x + ((px * scale) >> 8);
                    pairRun(row, a, x + (((px + len) * scale) >> 8) - a, pair[b & 15], k.x0, k.x1);
                }
                px += len;
            }
        }
    }
}

CHGAME_RAMFUNC(spriterot) static void rotSpan(const uint8_t *src, int sw, int sh, int x0, int x1, int y,
                                              int32_t u, int32_t v, int32_t du, int32_t dv,
                                              const uint8_t *remap) {
    uint8_t *row = gfx_fb + y * GFX_FB_STRIDE;
    for (int x = x0; x < x1; x++, u += du, v += dv) {
        int sx = u >> 16, sy = v >> 16;
        if ((unsigned)sx >= (unsigned)sw || (unsigned)sy >= (unsigned)sh) continue;
        uint8_t b = src[sy * ((sw + 1) >> 1) + (sx >> 1)];
        uint8_t c = (sx & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15);
        if (c != 15) plot(row + (x >> 1), x, remap ? remap[c] : c);
    }
}

void rotRaw(int w, int h, int ax, int ay, int px, int py, uint8_t angle, int scale, const uint8_t *remap) {
    const uint8_t *buf = gfx_chunkScratch();
    if (scale <= 0) return;
    // Inverse map: screen offset (dx, dy) from the pivot -> source pixel.
    int cs = fx::isin(angle + 64), sn = fx::isin(angle);            // Q8
    int32_t ic = (int32_t)cs * 256 / scale, is = (int32_t)sn * 256 / scale;   // Q8, divided by scale
    // rotSpan draws (dx, dy) from the pivot only where ic*dx + is*dy is in
    // [-256ax - 128, 256(w - ax) - 128) and ic*dy - is*dx in [-256ay - 128,
    // 256(h - ay) - 128): a parallelogram, centre (cx, cy) / D and half-size
    // (ex, ey) / D. Visit just its box, not the square round the pivot.
    int32_t D = ic * ic + is * is, mA = 128 * (w - 2 * ax - 1), mB = 128 * (h - 2 * ay - 1);
    int32_t aic = ic < 0 ? -ic : ic, ais = is < 0 ? -is : is;
    int32_t cx = ic * mA - is * mB, ex = 128 * (aic * w + ais * h);
    int32_t cy = is * mA + ic * mB, ey = 128 * (ais * w + aic * h);
    int x0 = px + (int)((cx - ex) / D), x1 = px + (int)((cx + ex) / D) + 1;
    int y0 = py + (int)((cy - ey) / D), y1 = py + (int)((cy + ey) / D) + 1;
    const GfxClip &k = CLIP;
    if (y0 < k.y0) y0 = k.y0;
    if (y1 > k.y1) y1 = k.y1;
    if (x0 < k.x0) x0 = k.x0;
    if (x1 > k.x1) x1 = k.x1;
    for (int y = y0; y < y1; y++) {
        int dy = y - py, dx = x0 - px;
        // src = R(-angle) * (dx, dy) / scale + pivot, in 16.16
        int32_t u = ((ic * dx + is * dy) << 8) + ((int32_t)ax << 16) + 0x8000;
        int32_t v = ((-is * dx + ic * dy) << 8) + ((int32_t)ay << 16) + 0x8000;
        rotSpan(buf, w, h, x0, x1, y, u, v, ic << 8, -is << 8, remap);
    }
}

void spriteRot(const uint8_t *d, int ax, int ay, int px, int py, uint8_t angle, int scale,
               const uint8_t *remap) {
    int w = d[0], h = d[1], stride = (w + 1) >> 1;
    if (stride * h > 1024 || scale <= 0) return;
    // Decode to raw 4 bpp (15 = transparent).
    uint8_t *buf = gfx_chunkScratch();
    memset(buf, 0xFF, stride * h);
    const uint8_t *p = d + 2;
    for (int j = 0; j < h; j++) {
        uint8_t n = *p++;
        int x = 0;
        while (n--) {
            uint8_t b = *p++;
            int len = (b >> 4) + 1;
            uint8_t c = b & 15;
            for (int k = 0; k < len; k++, x++) {
                uint8_t &q = buf[j * stride + (x >> 1)];
                q = (x & 1) ? (uint8_t)((q & 0x0F) | (c << 4)) : (uint8_t)((q & 0xF0) | c);
            }
        }
    }
    rotRaw(w, h, ax, ay, px, py, angle, scale, remap);
}

// ---------------------------------------------------------------------------
// PPOT Font3x5 (Press Play On Tape, Apache-2.0). Column bytes, bit 0 = top,
// bit 5 = descender, one glyph for each character from '!' to 'z' (blank
// where there is none). Extended here with $ , / ' * ( ) < > = % # " & ; _.
// ---------------------------------------------------------------------------
enum : char { FONT35_FIRST = '!', FONT35_LAST = 'z' };
static const uint8_t FONT35[FONT35_LAST - FONT35_FIRST + 1][3] = {
    {0x00,0x17,0x00},{0x03,0x00,0x03},{0x1F,0x0A,0x1F},{0x12,0x1F,0x09},{0x19,0x04,0x13},{0x0A,0x15,0x1A},{0x00,0x03,0x00},{0x00,0x0E,0x11},  // ! " # $ % & ' (
    {0x11,0x0E,0x00},{0x0A,0x04,0x0A},{0x04,0x0E,0x04},{0x20,0x10,0x00},{0x04,0x04,0x04},{0x00,0x10,0x00},{0x18,0x06,0x01},{0x1F,0x11,0x1F},  // ) * + , - . / 0
    {0x12,0x1F,0x10},{0x1D,0x15,0x17},{0x11,0x15,0x1F},{0x07,0x04,0x1F},{0x17,0x15,0x1D},{0x1F,0x15,0x1D},{0x01,0x01,0x1F},{0x1F,0x15,0x1F},  // 1 2 3 4 5 6 7 8
    {0x17,0x15,0x1F},{0x0A,0x00,0x00},{0x20,0x1A,0x00},{0x04,0x0A,0x11},{0x0A,0x0A,0x0A},{0x11,0x0A,0x04},{0x02,0x29,0x06},{0x00,0x00,0x00},  // 9 : ; < = > ? @
    {0x1F,0x05,0x1F},{0x1F,0x15,0x1B},{0x1F,0x11,0x11},{0x1F,0x11,0x0E},{0x1F,0x15,0x11},{0x1F,0x05,0x01},{0x1F,0x11,0x1D},{0x1F,0x04,0x1F},  // A B C D E F G H
    {0x00,0x1F,0x00},{0x10,0x10,0x1F},{0x1F,0x04,0x1B},{0x1F,0x10,0x10},{0x1F,0x06,0x1F},{0x1F,0x01,0x1F},{0x1F,0x11,0x1F},{0x1F,0x05,0x07},  // I J K L M N O P
    {0x1F,0x31,0x1F},{0x1F,0x05,0x1B},{0x17,0x15,0x1D},{0x01,0x1F,0x01},{0x1F,0x10,0x1F},{0x0F,0x10,0x0F},{0x1F,0x0C,0x1F},{0x1B,0x04,0x1B},  // Q R S T U V W X
    {0x07,0x1C,0x07},{0x19,0x15,0x13},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x10,0x10,0x10},{0x00,0x00,0x00},  // Y Z [ \ ] ^ _ `
    {0x0C,0x12,0x1E},{0x1F,0x12,0x0C},{0x1E,0x12,0x12},{0x0C,0x12,0x1F},{0x0C,0x1A,0x14},{0x04,0x1F,0x05},{0x2E,0x2A,0x1E},{0x1F,0x02,0x1C},  // a b c d e f g h
    {0x00,0x1D,0x00},{0x20,0x1D,0x00},{0x1F,0x04,0x1A},{0x01,0x1F,0x00},{0x1E,0x04,0x1E},{0x1E,0x02,0x1E},{0x1E,0x12,0x1E},{0x3E,0x12,0x0C},  // i j k l m n o p
    {0x0C,0x12,0x3E},{0x1E,0x02,0x06},{0x14,0x12,0x0A},{0x02,0x0F,0x12},{0x1E,0x10,0x1E},{0x0E,0x10,0x0E},{0x1E,0x08,0x1E},{0x1A,0x04,0x1A},  // q r s t u v w x
    {0x2E,0x28,0x1E},{0x1A,0x12,0x16},                                                                                                      // y z
};

const uint8_t *glyph35(char ch) {
    return ch >= FONT35_FIRST && ch <= FONT35_LAST ? FONT35[ch - FONT35_FIRST] : nullptr;
}

// Column-major glyph, bit 0 = top row: the layout of both PPOT's font and
// CHGfx's built-in one.
CHGAME_RAMFUNC(glyph) void glyph(int x, int y, const uint8_t *cols, uint8_t ncols, uint8_t c) {
    const GfxClip &k = CLIP;
    c &= 0x0F;
    for (uint8_t i = 0; i < ncols; i++, x++) {
        uint8_t bits = cols[i];
        if (!bits || x < k.x0 || x >= k.x1) continue;
        int yy = y;
        uint8_t *p = gfx_fb + yy * GFX_FB_STRIDE + (x >> 1);
        for (; bits; bits >>= 1, yy++, p += GFX_FB_STRIDE)
            if ((bits & 1) && yy >= k.y0 && yy < k.y1) plot(p, x, c);
    }
}

CHGAME_RAMFUNC(glyph16) void glyph16(int x, int y, const uint16_t *rows, uint8_t nrows, uint8_t c) {
    const GfxClip &k = CLIP;
    c &= 0x0F;
    for (uint8_t r = 0; r < nrows; r++, y++) {
        uint32_t bits = rows[r];
        if (!bits || y < k.y0 || y >= k.y1) continue;
        uint8_t *row = gfx_fb + y * GFX_FB_STRIDE;
        for (int xx = x; bits; bits = (bits << 1) & 0xFFFF, xx++)
            if ((bits & 0x8000) && xx >= k.x0 && xx < k.x1) plot(row + (xx >> 1), xx, c);
    }
}

CHGAME_RAMFUNC(text35) int text35(int x, int y, const char *str, uint8_t c) {
    int x0 = x;
    for (; *str; str++) {
        char ch = *str;
        if (ch == '\n') { x = x0; y += 7; continue; }
        if (ch == '~') { x += 2; continue; }
        const uint8_t *g = glyph35(ch);
        if (g) glyph(x, y, g, 3, c);
        x += 4;
    }
    return x - x0;
}

int text35s(int x, int y, const char *str, uint8_t c, uint8_t shade) {
    text35(x + 1, y + 1, str, shade);
    return text35(x, y, str, c);
}

// Each font pixel a 2x2 block (gfx_fillRect, which clips). From flash: it
// is for menus and titles, not for every frame.
void text35x2(int x, int y, const char *str, uint8_t c) {
    int x0 = x;
    for (; *str; str++) {
        char ch = *str;
        if (ch == '\n') { x = x0; y += 14; continue; }
        if (ch == '~') { x += 4; continue; }
        const uint8_t *g = glyph35(ch);
        if (g)
            for (int i = 0; i < 3; i++)
                for (int r = 0, bits = g[i]; bits; r++, bits >>= 1)
                    if (bits & 1) gfx_fillRect(x + 2 * i, y + 2 * r, 2, 2, c);
        x += 8;
    }
}

void text35x2s(int x, int y, const char *str, uint8_t c, uint8_t shade) {
    text35x2(x + 1, y + 1, str, shade);
    text35x2(x, y, str, c);
}

int text35Width(const char *str) {
    int w = 0, best = 0;
    for (; *str; str++) {
        if (*str == '\n') { if (w > best) best = w; w = 0; }
        else w += (*str == '~') ? 2 : 4;
    }
    if (w > best) best = w;
    return best ? best - 1 : 0;
}
