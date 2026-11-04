// Sizzle's bodies. Included once per sketch, from its Fx.cpp, after the
// game's `#pragma GCC optimize` (which governs every function defined below)
// and after its Fx.h (the SIZZLE_* switches and <chgame/Sizzle.h>).
// See Sizzle.h for the switches. Nothing here is compiled by the library.
#ifndef SIZZLE_CONFIGURED
#error "include the game's Fx.h (its SIZZLE_* switches, then <chgame/Sizzle.h>) before <chgame/Sizzle.inl>"
#endif
#include <string.h>

namespace fx {

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------
struct Particle { int16_t x, y; int8_t vx, vy; uint8_t life, colour, kind, age; };
static Particle parts[SIZZLE_NO_PARTICLES ? 1 : SIZZLE_POOL];
#if SIZZLE_HUES_EXPORT
const uint8_t SIZZLE_HUES_NAME[5] = SIZZLE_HUES;          // the casino rainbow, for the game too
#endif
#if SIZZLE_COIN_FLOOR_RUNTIME
static int16_t floorY = SIZZLE_COIN_FLOOR << 4;       // where coins bounce (Q4)
#define SIZZLE_FLOOR_Q4 floorY

void setFloor(int y) { floorY = (int16_t)(y << 4); }
#else
#define SIZZLE_FLOOR_Q4 (SIZZLE_COIN_FLOOR << 4)
#endif

#if SIZZLE_NO_PARTICLES                                   // the API stays; nothing flies
void spawn(Kind, int, int, int, int, uint8_t, uint8_t) {}
#if SIZZLE_BURST
void burst(Kind, int, int, uint8_t, int, uint8_t) {}
#endif
#if SIZZLE_KIND_COIN
void fountain(Kind, int, int, uint8_t) {}
#else
void fountain(int, int, uint8_t) {}
#endif
bool particles() { return false; }
static void updateParticles() {}
#if SIZZLE_DUST == 3
void drawParticles(uint8_t) {}
#else
void drawParticles() {}
#endif
#else
void spawn(Kind k, int x, int y, int vx, int vy, uint8_t life, uint8_t colour) {
    Particle *slot = nullptr;
    for (auto &p : parts) if (!p.life) { slot = &p; break; }
    if (!slot) slot = &parts[rnd() % SIZZLE_POOL];        // steal one
    slot->x = (int16_t)(x << 4); slot->y = (int16_t)(y << 4);
    slot->vx = (int8_t)(vx < -127 ? -127 : vx > 127 ? 127 : vx);
    slot->vy = (int8_t)(vy < -127 ? -127 : vy > 127 ? 127 : vy);
    slot->life = life; slot->colour = colour; slot->kind = k; slot->age = 0;
}

#if SIZZLE_BURST
void burst(Kind k, int x, int y, uint8_t n, int speed, uint8_t colour) {
    for (uint8_t i = 0; i < n; i++) {
        int a = (int)(i * 256 / n) + rndRange(0, 12);
        int sp = speed / 2 + rndRange(0, speed / 2 + 1);
#if SIZZLE_DUST == 1                                      // (code shape) the pixel-dust games compute both in place
        spawn(k, x, y, (isin(a + 64) * sp) >> 8, (isin(a) * sp) >> 8, (uint8_t)rndRange(20, 40), colour);
#else
        int vy = (isin(a) * sp) >> 8;
#if SIZZLE_DUST_SPREAD
        if (k == DUST) vy /= 2;                           // puffs spread along the floor
#endif
        spawn(k, x, y, (isin(a + 64) * sp) >> 8, vy, (uint8_t)rndRange(20, 40), colour);
#endif
    }
}
#endif

#if SIZZLE_KIND_COIN
void fountain(Kind k, int x, int y, uint8_t n) {
    static const uint8_t CONF[6] = SIZZLE_CONFETTI_COLOURS;
    for (uint8_t i = 0; i < n; i++)
        spawn(k, x + rndRange(-4, 5), y, rndRange(-28, 29), rndRange(-60, -30), (uint8_t)rndRange(40, 70),
#if SIZZLE_FOUNTAIN_GOLD_COINS
              k == COIN ? GOLD : CONF[rnd() % 6]);
#else
              CONF[rnd() % 6]);
#endif
}
#else
void fountain(int x, int y, uint8_t n) {
    static const uint8_t CONF[6] = SIZZLE_CONFETTI_COLOURS;
    for (uint8_t i = 0; i < n; i++)
        spawn(CONFETTI, x + rndRange(-4, 5), y, rndRange(-28, 29), rndRange(-60, -30), (uint8_t)rndRange(40, 70),
              CONF[rnd() % 6]);
}
#endif

bool particles() {
    for (auto &p : parts) if (p.life) return true;
    return false;
}

static void updateParticles() {
    for (auto &p : parts) {
        if (!p.life) continue;
        p.life--; p.age++;
        p.x += p.vx; p.y += p.vy;
        switch (p.kind) {
            case CONFETTI: if (p.age & 1) p.vy += 2; if (p.vy > 24) p.vy = 24;
                           p.vx = (int8_t)(p.vx * 15 / 16); break;
#if SIZZLE_KIND_COIN && !SIZZLE_COIN_LATE
            case COIN:     p.vy += 3; if (p.y > SIZZLE_FLOOR_Q4) { p.vy = (int8_t)(-p.vy / 2); p.y = SIZZLE_FLOOR_Q4; } break;
#endif
#if SIZZLE_KIND_RAIN
            case SIZZLE_RAIN_KIND_NAME: break;
#endif
#if SIZZLE_KIND_DUST
            case DUST:     p.vx = (int8_t)(p.vx * 7 / 8); p.vy = (int8_t)(p.vy * 7 / 8); break;
#endif
#if SIZZLE_KIND_COIN && SIZZLE_COIN_LATE
            case COIN:     p.vy += 3; if (p.y > SIZZLE_FLOOR_Q4) { p.vy = (int8_t)(-p.vy / 2); p.y = SIZZLE_FLOOR_Q4; } break;
#endif
#if SIZZLE_KIND_GOO
            case GOO:      if (p.vy < 96) p.vy += 4; p.vx = (int8_t)(p.vx * 31 / 32); break;
#endif
            default:       p.vy += (p.age & 3) == 0; break;
        }
    }
}

#if SIZZLE_DUST == 3
void drawParticles(uint8_t dust) {
#else
void drawParticles() {
#if SIZZLE_DUST == 2
    const uint8_t dust = 2;
#endif
#endif
    for (auto &p : parts) {
        if (!p.life) continue;
        int x = p.x >> 4, y = p.y >> 4;
#if SIZZLE_COLOUR_INLINE
#define c p.colour
#else
        uint8_t c = p.colour;
#endif
        switch (p.kind) {
#if SIZZLE_KIND_SPARK
            case SPARK:
                gfx_pixel(x, y, c);
                if (p.age < 8) { gfx_pixel(x - 1, y, c); gfx_pixel(x + 1, y, c);
                                 gfx_pixel(x, y - 1, c); gfx_pixel(x, y + 1, c); }
                break;
#endif
            case CONFETTI:
                if ((p.age >> 2) & 1) gfx_hline(x, y, 2, c);
                else gfx_vline(x, y, 2, c);
                break;
#if SIZZLE_KIND_COIN && !SIZZLE_COIN_LATE
            case COIN: {                                  // spinning: wide, then edge on
                int w = ((p.age >> 2) & 3) == 2 ? 1 : 3;
                gfx_fillRect(x - w / 2, y - 1, w, 3, GOLD);
                gfx_pixel(x, y, w == 3 ? WOOD : GOLD);
                break;
            }
#endif
#if SIZZLE_KIND_RAIN
            case SIZZLE_RAIN_KIND_NAME: gfx_vline(x, y, 3, c); break;
#endif
#if SIZZLE_KIND_STAR
            case STAR:
                gfx_hline(x - 1, y, 3, c); gfx_vline(x, y - 1, 3, c);
                break;
#endif
#if SIZZLE_KIND_COIN && SIZZLE_COIN_LATE
            case COIN: {                                  // spinning: wide, then edge on
                int w = ((p.age >> 2) & 3) == 2 ? 1 : 3;
                gfx_fillRect(x - w / 2, y - 1, w, 3, GOLD);
                gfx_pixel(x, y, w == 3 ? WOOD : GOLD);
                break;
            }
#endif
#if SIZZLE_DUST == 1
            case DUST: if (p.life > 4 || (p.life & 1)) gfx_pixel(x, y, c); break;
#elif SIZZLE_DUST >= 2
            case DUST: {                                  // a puff, down to a speck, centred
                int s = p.life > 10 ? dust : (p.life > 4 || (p.life & 1)) ? (dust + 1) / 2 : 0;
                gfx_fillRect(x - s / 2, y - s / 2, s, s, c);
                break;
            }
#endif
#if SIZZLE_KIND_GOO
            case GOO: {                                   // a blob trailing a string of itself
                int tx = x - p.vx / 6, ty = y - p.vy / 6;
                gfx_line(x, y, tx, ty, c);
                if (p.life > 24) {                         // a fresh glob
                    gfx_fillRect(x - 2, y - 1, 4, 4, DARKER[c & 15]);
                    gfx_fillRect(x - 2, y - 2, 4, 4, c);
                    gfx_hline(x - 1, y - 2, 2, LIGHTER[c & 15]);
                } else if (p.life > 8) {
                    gfx_fillRect(x - 1, y, 3, 2, DARKER[c & 15]);
                    gfx_fillRect(x - 1, y - 1, 3, 2, c);
                    gfx_pixel(x - 1, y - 1, LIGHTER[c & 15]);
                } else gfx_fillRect(x, y, 2, 2, c);
                break;
            }
#endif
        }
    }
#if SIZZLE_COLOUR_INLINE
#undef c
#endif
}
#endif  // SIZZLE_NO_PARTICLES

// ---------------------------------------------------------------------------
// Banner
// ---------------------------------------------------------------------------
static char bannerText[SIZZLE_BANNER_CHARS];
#if SIZZLE_BANNER_DROP
static uint8_t bannerLen, bannerStyle, bannerT, bannerFrames;
#else
static uint8_t bannerStyle, bannerT, bannerFrames;
#endif
static int bannerCy;
#if SIZZLE_HOLD_BANNER
static bool bannerHeld;
#endif

void banner(const char *text, BannerStyle s, int cy, uint8_t frames) {
#if SIZZLE_BANNER_DROP
    bannerLen = (uint8_t)(fmtStr(bannerText, text) - bannerText);
#else
    strncpy(bannerText, text, sizeof bannerText - 1);
    bannerText[sizeof bannerText - 1] = 0;
#endif
    bannerStyle = s; bannerCy = cy; bannerT = 0; bannerFrames = frames;
#if SIZZLE_HOLD_BANNER
    bannerHeld = false;
#endif
}

#if SIZZLE_HOLD_BANNER
void holdBanner(bool on) { bannerHeld = on; }
#endif

bool bannerActive() { return bannerFrames != 0; }

// The rainbow's outline and shade, in a game that has the style (a game
// without it, such as CHStlView, gets the INK outline).
#if SIZZLE_STYLES & SIZZLE_RAINBOW
#define SIZZLE_IS_RAINBOW(s) ((s) == B_RAINBOW)
#else
#define SIZZLE_IS_RAINBOW(s) false
#endif

// Which style's ramp is the switch's default: WHITE, else the last one the
// game has (the cases come in the order RAINBOW, GOLD, RED, CYAN, BLACK, GREEN).
#if SIZZLE_STYLE_RAMPS & SIZZLE_WHITE
#define SIZZLE_DEFAULT_STYLE SIZZLE_WHITE
#elif SIZZLE_STYLE_RAMPS & SIZZLE_GREEN
#define SIZZLE_DEFAULT_STYLE SIZZLE_GREEN
#elif SIZZLE_STYLE_RAMPS & SIZZLE_BLACK
#define SIZZLE_DEFAULT_STYLE SIZZLE_BLACK
#elif SIZZLE_STYLE_RAMPS & SIZZLE_CYAN
#define SIZZLE_DEFAULT_STYLE SIZZLE_CYAN
#elif SIZZLE_STYLE_RAMPS & SIZZLE_RED
#define SIZZLE_DEFAULT_STYLE SIZZLE_RED
#else
#define SIZZLE_DEFAULT_STYLE SIZZLE_GOLD
#endif
#define SIZZLE_CASE(bit) ((SIZZLE_STYLE_RAMPS & (bit)) && SIZZLE_DEFAULT_STYLE != (bit))

#if SIZZLE_BANNER_DROP
// The letters drop in one after another from DROP px up, bounce as they
// land, then dance on the spot.
static const int DROP = 18, STAGGER = 2, FALL = 12;

static bool dropping() { return bannerT < STAGGER * bannerLen + FALL; }

// The ramps, by the letters' row g.
#define SIZZLE_RAMP_RAINBOW SIZZLE_HUES_NAME[(((r + 8) / 2) + t / 3) % 5]
#define SIZZLE_RAMP_GOLD    (g < 3 ? FX_B : (g < 8 ? GOLD : WOOD))
#define SIZZLE_RAMP_RED     (g < 3 ? WHITE : (g < 9 ? RED : WINE))
#define SIZZLE_RAMP_CYAN    (g < 3 ? WHITE : SIZZLE_CYAN_INK)
#define SIZZLE_RAMP_WHITE   (g < 6 ? WHITE : SILVER)
#define SIZZLE_RAMP_BLACK   (g < 3 ? WHITE : (g < 9 ? SILVER : NAVY))
#define SIZZLE_RAMP_GREEN   (g < 3 ? WHITE : (g < 9 ? FELT_LT : FELT))
#else
// The ramps, by the mask's row r (h: the letters' height).
#define SIZZLE_RAMP_RAINBOW SIZZLE_HUES_NAME[((r / 2) + t / 3) % 5]
#define SIZZLE_RAMP_GOLD    (r < 3 ? FX_B : (r < h / 2 + 6 ? GOLD : WOOD))
#define SIZZLE_RAMP_RED     (r < 3 ? WHITE : RED)
#define SIZZLE_RAMP_CYAN    (r < 3 ? WHITE : CYAN)
#define SIZZLE_RAMP_WHITE   WHITE
#define SIZZLE_RAMP_BLACK   (r < 3 ? WHITE : (r < h / 2 + 3 ? SILVER : NAVY))
#define SIZZLE_RAMP_GREEN   (r < 3 ? WHITE : (r < h / 2 + 6 ? FELT_LT : FELT))
#endif
#if SIZZLE_DEFAULT_STYLE == SIZZLE_WHITE
#define SIZZLE_RAMP_DEFAULT SIZZLE_RAMP_WHITE
#elif SIZZLE_DEFAULT_STYLE == SIZZLE_GREEN
#define SIZZLE_RAMP_DEFAULT SIZZLE_RAMP_GREEN
#elif SIZZLE_DEFAULT_STYLE == SIZZLE_BLACK
#define SIZZLE_RAMP_DEFAULT SIZZLE_RAMP_BLACK
#elif SIZZLE_DEFAULT_STYLE == SIZZLE_CYAN
#define SIZZLE_RAMP_DEFAULT SIZZLE_RAMP_CYAN
#elif SIZZLE_DEFAULT_STYLE == SIZZLE_RED
#define SIZZLE_RAMP_DEFAULT SIZZLE_RAMP_RED
#else
#define SIZZLE_RAMP_DEFAULT SIZZLE_RAMP_GOLD
#endif


void drawBanner() {
    if (!bannerFrames) return;
    int t = bannerT;
#if !SIZZLE_HUES_EXPORT
    static const uint8_t SIZZLE_HUES_NAME[5] = SIZZLE_HUES;
#endif
#if SIZZLE_BANNER_DROP
    uint8_t gap = 1;
    int w = SIZZLE_FONT_WIDTH(bannerText, gap);
    if (w > 124) w = SIZZLE_FONT_WIDTH(bannerText, gap = 0);
    int n = bannerLen, off = (dropping() ? DROP : 0) + 2;
    int8_t dy[SIZZLE_BANNER_CHARS];
    for (int k = 0; k < n && k < SIZZLE_BANNER_CHARS; k++) {
        int tk = t - STAGGER * k, d = 0;
        if (tk < 0) d = -60;                                            // not yet: out of the mask
        else if (tk < FALL) d = -(((256 - ease(OUT_BOUNCE, tk, FALL)) * DROP) >> 8);
        dy[k] = (int8_t)(d + off + ((isin(t * 10 + k * 36) * 2) >> 8));
    }
    int h = SIZZLE_FONT_H + off + 2;
    Mask m = maskBegin(w + 1, h);
    SIZZLE_FONT_MASK(m, 0, 0, bannerText, dy, gap);
    // Last few frames: blink out.
    if (bannerFrames < 10 && (bannerFrames & 2)) return;
    uint8_t ramp[40];
    for (int r = 0; r < h && r < 40; r++) {
        int g = r - off;                                                // row of the letters
        (void)g;                                                        // (only some ramps read it)
        switch (bannerStyle) {                       // a case for each style the game has, the default's left out
#if SIZZLE_CASE(SIZZLE_RAINBOW)
            case B_RAINBOW: ramp[r] = SIZZLE_RAMP_RAINBOW; break;
#endif
#if SIZZLE_CASE(SIZZLE_GOLD)
            case B_GOLD:    ramp[r] = SIZZLE_RAMP_GOLD; break;
#endif
#if SIZZLE_CASE(SIZZLE_RED)
            case B_RED:     ramp[r] = SIZZLE_RAMP_RED; break;
#endif
#if SIZZLE_CASE(SIZZLE_CYAN)
            case B_CYAN:    ramp[r] = SIZZLE_RAMP_CYAN; break;
#endif
#if SIZZLE_CASE(SIZZLE_BLACK)
            case B_BLACK:   ramp[r] = SIZZLE_RAMP_BLACK; break;
#endif
#if SIZZLE_CASE(SIZZLE_GREEN)
            case B_GREEN:   ramp[r] = SIZZLE_RAMP_GREEN; break;
#endif
            default:        ramp[r] = SIZZLE_RAMP_DEFAULT; break;
        }
    }
    uint8_t outline = SIZZLE_IS_RAINBOW(bannerStyle) ? FX_A : INK;
    maskDraw(m, 64 - w / 2, bannerCy - SIZZLE_FONT_H / 2 - off, SIZZLE_BANNER_FILL, outline, outline == INK ? -1 : INK, ramp);
    SIZZLE_BANNER_AFTER(64 - w / 2, bannerCy - SIZZLE_FONT_H / 2 - off, dy, gap);
#else
    uint8_t scale = t < 3 ? 2 : (t < 7 ? 4 : 3);
    int w = text35WidthScaled(bannerText, scale);
    while (w > 124 && scale > 2) w = text35WidthScaled(bannerText, --scale);
    int h = 6 * scale;
    int8_t dy[SIZZLE_BANNER_CHARS];
    int n = (int)strlen(bannerText);
    for (int k = 0; k < n && k < SIZZLE_BANNER_CHARS; k++) dy[k] = (int8_t)((isin(t * 10 + k * 36) * 2) >> 8) + 2;
    Mask m = maskBegin(w + 1, h + 5);
    maskText35(m, 0, 0, bannerText, scale, dy);
    // Last few frames: blink out.
    if (bannerFrames < 10 && (bannerFrames & 2)) return;
    uint8_t ramp[32];
    for (int r = 0; r < h + 5 && r < 32; r++) {
        switch (bannerStyle) {                       // a case for each style the game has, the default's left out
#if SIZZLE_CASE(SIZZLE_RAINBOW)
            case B_RAINBOW: ramp[r] = SIZZLE_RAMP_RAINBOW; break;
#endif
#if SIZZLE_CASE(SIZZLE_GOLD)
            case B_GOLD:    ramp[r] = SIZZLE_RAMP_GOLD; break;
#endif
#if SIZZLE_CASE(SIZZLE_RED)
            case B_RED:     ramp[r] = SIZZLE_RAMP_RED; break;
#endif
#if SIZZLE_CASE(SIZZLE_CYAN)
            case B_CYAN:    ramp[r] = SIZZLE_RAMP_CYAN; break;
#endif
#if SIZZLE_CASE(SIZZLE_BLACK)
            case B_BLACK:   ramp[r] = SIZZLE_RAMP_BLACK; break;
#endif
#if SIZZLE_CASE(SIZZLE_GREEN)
            case B_GREEN:   ramp[r] = SIZZLE_RAMP_GREEN; break;
#endif
            default:        ramp[r] = SIZZLE_RAMP_DEFAULT; break;
        }
    }
    uint8_t outline = SIZZLE_IS_RAINBOW(bannerStyle) ? FX_A : INK;
    maskDraw(m, 64 - w / 2, bannerCy - h / 2 - 2, SIZZLE_BANNER_FILL, outline, SIZZLE_IS_RAINBOW(bannerStyle) ? INK : WINE, ramp);
#endif
}

// ---------------------------------------------------------------------------
// Floating text ("+$15", rising)
// ---------------------------------------------------------------------------
#if SIZZLE_FLOATS
struct Float { int16_t x, y; uint8_t t, colour; char text[SIZZLE_FLOAT_CHARS]; };
static Float floats[4];

void floatText(const char *text, int x, int y, uint8_t colour) {
    Float *f = &floats[0];
    for (auto &q : floats) if (!q.t) { f = &q; break; }
    f->x = (int16_t)x; f->y = (int16_t)y; f->t = 50; f->colour = colour;
    strncpy(f->text, text, sizeof f->text - 1); f->text[sizeof f->text - 1] = 0;
}

void drawFloats() {
    for (auto &f : floats) {
#if SIZZLE_FLOAT_BLINK_FIRST
        if (!f.t || (f.t < 8 && (f.t & 1))) continue;       // blinks out
#else
        if (!f.t) continue;
#endif
        int y = f.y - (50 - f.t) / 2;
        int x = f.x - text35Width(f.text) / 2;
#if SIZZLE_FLOAT_CLAMP
        if (x < 1) x = 1;
        if (x + text35Width(f.text) > 127) x = 127 - text35Width(f.text);
#endif
#if !SIZZLE_FLOAT_BLINK_FIRST
        if (f.t < 8 && (f.t & 1)) continue;
#endif
        text35(x + 1, y + 1, f.text, INK);
        text35(x, y, f.text, f.colour);
    }
}
#endif

bool activeRows(int &lo, int &hi) {
    lo = 999; hi = -1;
#if SIZZLE_SHAKE
    if (shaking()) { lo = 0; hi = 127; return true; }
#endif
#if SIZZLE_KIND_GOO
    for (auto &p : parts) if (p.life) {
        int y = p.y >> 4, a = y, b = y;
        if (p.kind == GOO) { int t = y - p.vy / 6; if (t < a) a = t; else b = t; }   // its string
        if (a - 2 < lo) lo = a - 2;
        if (b + 3 > hi) hi = b + 3;
    }
#else
    for (auto &p : parts) if (p.life) { int y = p.y >> 4; if (y - 2 < lo) lo = y - 2; if (y + 3 > hi) hi = y + 3; }
#endif
#if SIZZLE_FLOATS
    for (auto &f : floats) if (f.t) { int y = f.y - (50 - f.t) / 2; if (y - 1 < lo) lo = y - 1; if (y + 7 > hi) hi = y + 7; }
#endif
    if (bannerFrames) { if (bannerCy - SIZZLE_BANNER_ROWS_UP < lo) lo = bannerCy - SIZZLE_BANNER_ROWS_UP; if (bannerCy + SIZZLE_BANNER_ROWS_DOWN > hi) hi = bannerCy + SIZZLE_BANNER_ROWS_DOWN; }
    return hi >= lo;
}

void clear() {
    memset(parts, 0, sizeof parts);
#if SIZZLE_FLOATS
    memset(floats, 0, sizeof floats);
#endif
    bannerFrames = 0;
#if SIZZLE_HOLD_BANNER
    bannerHeld = false;
#endif
#if SIZZLE_SHAKE
    shakeStop();
#endif
}

void update() {
    updateParticles();
    if (bannerFrames) {
#if SIZZLE_HOLD_BANNER
        if (!bannerHeld || bannerFrames > 10) bannerFrames--;    // held: up, until let go to blink out
#else
        bannerFrames--;
#endif
#if SIZZLE_BANNER_WRAP
        if (!++bannerT) bannerT = 128;                           // (the same phase of the dance)
#else
        bannerT++;
#endif
    }
#if SIZZLE_FLOATS
    for (auto &f : floats) if (f.t) f.t--;
#endif
#if SIZZLE_SHAKE
    shakeTick();
#endif
}

}  // namespace fx
