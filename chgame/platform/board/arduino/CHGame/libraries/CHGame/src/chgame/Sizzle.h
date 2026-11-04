/// @file Sizzle.h
/// @brief Sizzle: the particle pool, the pop-up banner and the floating "+$15"
///        texts the games share, on top of fx:: (chgame/Fx.h).
///
/// This is an implementation header, not part of <CHGame.h>. See
/// @ref chgame_sizzle for how a game configures and compiles it.
#pragma once
#include <stdint.h>
#include <CHGame.h>

/// @defgroup chgame_sizzle Particles, banners and floating texts
/// @ingroup lib_chgame
/// @brief fx:: particles (sparks, confetti, coins ...), the pop-up banner and
///        floating texts, configured per game.
///
/// The library is compiled apart from a sketch and sees none of its defines,
/// and the twenty games differ in what they need (how many particles, which
/// kinds, which banner, floats or not) and in the size tuning of the file
/// that holds the code. So a game configures it in its own src/fx/Fx.h and
/// the bodies are compiled in the game's src/fx/Fx.cpp:
///
/// @code
/// // src/fx/Fx.h
/// #pragma once
/// #include <CHGame.h>
/// #define SIZZLE_CONFIGURED 1
/// #define SIZZLE_POOL 64            // only what differs from the defaults below
/// #include <chgame/Sizzle.h>
/// namespace fx { void explode(int x, int y, uint8_t n); }     // the game's own extras, if any
///
/// // src/fx/Fx.cpp
/// #pragma GCC optimize("Os")        // line 1: it governs the bodies included below
/// #include "Fx.h"
/// #include <chgame/Sizzle.inl>
/// @endcode
///
/// Every call site stays fx::burst(), fx::banner() and so on. Each frame:
/// fx::update() once per logic tick; when drawing, fx::drawParticles(),
/// fx::drawFloats() and fx::drawBanner() on top of the scene.
///
/// Positions are pixels; particle velocities are in 1/16 px per tick. This
/// page documents the default configuration (the kinds SPARK, CONFETTI, STAR
/// and DUST; the five banner styles; floats on).
///
/// The switches (the games' own are listed in docs/chgame-library.md):
///
/// | Switch | Default | Meaning |
/// |---|---|---|
/// | `SIZZLE_POOL` | 48 | particles in the pool |
/// | `SIZZLE_KIND_SPARK` / `_STAR` / `_DUST` | 1 | the kinds that exist (CONFETTI always does) |
/// | `SIZZLE_KIND_COIN` | 0 | bouncing coins; also fountain(Kind, ...) instead of fountain(x, y, n) |
/// | `SIZZLE_KIND_RAIN` | 0 | falling drops, named SIZZLE_RAIN_KIND_NAME (RAIN_DROP; RAIN in the card games, whose hue table is then not exported under that name) |
/// | `SIZZLE_KIND_GOO` | 0 | CHBingo's ink blobs |
/// | `SIZZLE_KIND_ORDER` | - | the enumerators in another order (CHBoardwalk: COIN last) |
/// | `SIZZLE_BURST` | 1 | burst() exists |
/// | `SIZZLE_COLOUR_INLINE` | 0 | (code shape) drawParticles() reads p.colour in place instead of a local |
/// | `SIZZLE_COIN_LATE` | 0 | (code shape) the COIN case comes after DUST and STAR, as in CHBoardwalk |
/// | `SIZZLE_DUST` | 3 | how DUST draws: 1 one pixel, 2 a puff of size 2, 3 a puff of the size drawParticles(dust) is given (default 2) |
/// | `SIZZLE_FOUNTAIN_GOLD_COINS` | = COIN | fountain(COIN, ...) spawns gold coins (else confetti colours) |
/// | `SIZZLE_COIN_FLOOR` | 122 | the row coins bounce on; `SIZZLE_COIN_FLOOR_RUNTIME` 1 adds setFloor() |
/// | `SIZZLE_HUES` | {RED, GOLD, FELT_LT, CYAN, BLUE} | the casino rainbow |
/// | `SIZZLE_HUES_EXPORT` | 1 | exported as `extern const uint8_t SIZZLE_HUES_NAME[5]` (RAIN) |
/// | `SIZZLE_CONFETTI_COLOURS` | {RED, GOLD, FELT_LT, CYAN, BLUE, WHITE} | confetti's colours |
/// | `SIZZLE_NO_PARTICLES` | 0 | 1: spawn(), burst(), fountain() and drawParticles() do nothing (a game's device debug build sets it to make room) |
/// | `SIZZLE_FLOATS` | 1 | floatText()/drawFloats(); `SIZZLE_FLOAT_CHARS` 8 (the text, with its 0), `SIZZLE_FLOAT_CLAMP` 0 (keep it on screen), `SIZZLE_FLOAT_BLINK_FIRST` 0 |
/// | `SIZZLE_BANNER_DROP` | 0 | 0: letters of the 3x5 font pop in at 2x, 4x, 3x (maskText35); 1: the game's display font, letters dropping in one by one (the game provides maskFont(), fontWidth() and FONT_H, or `SIZZLE_FONT_MASK`/`SIZZLE_FONT_WIDTH`/`SIZZLE_FONT_H`) |
/// | `SIZZLE_BANNER_CHARS` | 14 | the banner's text, with its 0 |
/// | `SIZZLE_BANNER_ROWS_UP` / `_DOWN` | 18/18 (drop: 30/12) | rows around the centre activeRows() reports |
/// | `SIZZLE_STYLES` | RAINBOW\|GOLD\|RED\|CYAN\|WHITE | the BannerStyle enumerators (`SIZZLE_BLACK`, `SIZZLE_GREEN` too) |
/// | `SIZZLE_STYLE_RAMPS` | = STYLES | the styles with a colour ramp of their own (the rest draw as the default) |
/// | `SIZZLE_CYAN_INK` | CYAN | (drop) the B_CYAN ink |
/// | `SIZZLE_BANNER_FILL` | 0 | the mask's fill (unused under a ramp; kept per game for identical code) |
/// | `SIZZLE_HOLD_BANNER` | 1 | holdBanner() |
/// | `SIZZLE_BANNER_WRAP` | 1 | the dance phase wraps to 128 (kept per game for identical code) |
/// | `SIZZLE_SHAKE` | 1 | the screen shake joins activeRows(), update() and clear() |
/// | `SIZZLE_BANNER_AFTER(x, y, dy, gap)` | - | (drop) a pass after the banner is drawn (CHDominoes' half ink) |
/// @{

#ifndef SIZZLE_CONFIGURED
#error "include the game's src/fx/Fx.h (its SIZZLE_* switches, then <chgame/Sizzle.h>), not this header alone"
#endif

// ---------------------------------------------------------------- defaults
#ifndef SIZZLE_POOL
#define SIZZLE_POOL 48
#endif
#ifndef SIZZLE_KIND_SPARK
#define SIZZLE_KIND_SPARK 1
#endif
#ifndef SIZZLE_KIND_STAR
#define SIZZLE_KIND_STAR 1
#endif
#ifndef SIZZLE_KIND_DUST
#define SIZZLE_KIND_DUST 1
#endif
#ifndef SIZZLE_KIND_COIN
#define SIZZLE_KIND_COIN 0
#endif
#ifndef SIZZLE_KIND_RAIN
#define SIZZLE_KIND_RAIN 0
#endif
#ifndef SIZZLE_RAIN_KIND_NAME
#define SIZZLE_RAIN_KIND_NAME RAIN_DROP
#endif
#ifndef SIZZLE_KIND_GOO
#define SIZZLE_KIND_GOO 0
#endif
#ifndef SIZZLE_BURST
#define SIZZLE_BURST 1
#endif
#ifndef SIZZLE_COLOUR_INLINE
#define SIZZLE_COLOUR_INLINE 0
#endif
#ifndef SIZZLE_COIN_LATE
#define SIZZLE_COIN_LATE 0
#endif
#ifndef SIZZLE_DUST
#define SIZZLE_DUST 3
#endif
#ifndef SIZZLE_FOUNTAIN_GOLD_COINS
#define SIZZLE_FOUNTAIN_GOLD_COINS SIZZLE_KIND_COIN
#endif
#ifndef SIZZLE_COIN_FLOOR
#define SIZZLE_COIN_FLOOR 122
#endif
#ifndef SIZZLE_COIN_FLOOR_RUNTIME
#define SIZZLE_COIN_FLOOR_RUNTIME 0
#endif
#ifndef SIZZLE_HUES
#define SIZZLE_HUES {RED, GOLD, FELT_LT, CYAN, BLUE}
#endif
#ifndef SIZZLE_HUES_EXPORT
#define SIZZLE_HUES_EXPORT 1
#endif
#ifndef SIZZLE_HUES_NAME
#define SIZZLE_HUES_NAME RAIN
#endif
#ifndef SIZZLE_CONFETTI_COLOURS
#define SIZZLE_CONFETTI_COLOURS {RED, GOLD, FELT_LT, CYAN, BLUE, WHITE}
#endif
#ifndef SIZZLE_NO_PARTICLES
#define SIZZLE_NO_PARTICLES 0
#endif
#ifndef SIZZLE_FLOATS
#define SIZZLE_FLOATS 1
#endif
#ifndef SIZZLE_FLOAT_CHARS
#define SIZZLE_FLOAT_CHARS 8
#endif
#ifndef SIZZLE_FLOAT_CLAMP
#define SIZZLE_FLOAT_CLAMP 0
#endif
#ifndef SIZZLE_FLOAT_BLINK_FIRST
#define SIZZLE_FLOAT_BLINK_FIRST 0
#endif
#ifndef SIZZLE_BANNER_DROP
#define SIZZLE_BANNER_DROP 0
#endif
#ifndef SIZZLE_BANNER_CHARS
#define SIZZLE_BANNER_CHARS 14
#endif
#ifndef SIZZLE_BANNER_ROWS_UP
#define SIZZLE_BANNER_ROWS_UP (SIZZLE_BANNER_DROP ? 30 : 18)
#endif
#ifndef SIZZLE_BANNER_ROWS_DOWN
#define SIZZLE_BANNER_ROWS_DOWN (SIZZLE_BANNER_DROP ? 12 : 18)
#endif
#define SIZZLE_RAINBOW 1
#define SIZZLE_GOLD 2
#define SIZZLE_RED 4
#define SIZZLE_CYAN 8
#define SIZZLE_WHITE 16
#define SIZZLE_BLACK 32
#define SIZZLE_GREEN 64
#ifndef SIZZLE_STYLES
#define SIZZLE_STYLES (SIZZLE_RAINBOW | SIZZLE_GOLD | SIZZLE_RED | SIZZLE_CYAN | SIZZLE_WHITE)
#endif
#ifndef SIZZLE_STYLE_RAMPS
#define SIZZLE_STYLE_RAMPS SIZZLE_STYLES
#endif
#ifndef SIZZLE_CYAN_INK
#define SIZZLE_CYAN_INK CYAN
#endif
#ifndef SIZZLE_BANNER_FILL
#define SIZZLE_BANNER_FILL 0
#endif
#ifndef SIZZLE_HOLD_BANNER
#define SIZZLE_HOLD_BANNER 1
#endif
#ifndef SIZZLE_BANNER_WRAP
#define SIZZLE_BANNER_WRAP 1
#endif
#ifndef SIZZLE_SHAKE
#define SIZZLE_SHAKE 1
#endif
#ifndef SIZZLE_FONT_MASK
#define SIZZLE_FONT_MASK(m, x, y, s, dy, gap) maskFont(m, x, y, s, dy, gap)
#endif
#ifndef SIZZLE_FONT_WIDTH
#define SIZZLE_FONT_WIDTH(s, gap) fontWidth(s, gap)
#endif
#ifndef SIZZLE_FONT_H
#define SIZZLE_FONT_H FONT_H
#endif
#ifndef SIZZLE_BANNER_AFTER
#define SIZZLE_BANNER_AFTER(x, y, dy, gap)
#endif
// DUST puffs spread along the floor in burst(); the pixel kind does not.
#define SIZZLE_DUST_SPREAD (SIZZLE_KIND_DUST && SIZZLE_DUST >= 2)
#if !SIZZLE_KIND_DUST
#undef SIZZLE_DUST
#define SIZZLE_DUST 0
#endif

#define SIZZLE_CAT_(a, b) a##b
#define SIZZLE_CAT(a, b) SIZZLE_CAT_(a, b)
#define SIZZLE_WORD_RAIN 1
#define SIZZLE_WORD_RAIN_DROP 2
#define SIZZLE_WORD_HUES 3
#if SIZZLE_KIND_RAIN && SIZZLE_HUES_EXPORT && \
    SIZZLE_CAT(SIZZLE_WORD_, SIZZLE_RAIN_KIND_NAME) == SIZZLE_CAT(SIZZLE_WORD_, SIZZLE_HUES_NAME)
#error "SIZZLE_RAIN_KIND_NAME and SIZZLE_HUES_NAME are the same word: name the kind RAIN_DROP, or export the hues as HUES"
#endif

namespace fx {

// ---------------------------------------------------------------- particles
#ifdef SIZZLE_KIND_ORDER
enum Kind : uint8_t { SIZZLE_KIND_ORDER };
#else
/// @brief The particle kinds (the game's switches decide which exist).
enum Kind : uint8_t {
#if SIZZLE_KIND_SPARK
    SPARK,          ///< A pixel with a cross-shaped flash for its first 8 ticks, falling slowly.
#endif
    CONFETTI,       ///< A 2 px scrap that flips flat and upright as it falls. Always present.
#if SIZZLE_KIND_COIN
    COIN,
#endif
#if SIZZLE_KIND_RAIN
    SIZZLE_RAIN_KIND_NAME,
#endif
#if SIZZLE_KIND_STAR
    STAR,           ///< A small plus sign, falling slowly.
#endif
#if SIZZLE_KIND_DUST
    DUST,           ///< A puff that slows to a stop and shrinks to a speck (see SIZZLE_DUST).
#endif
#if SIZZLE_KIND_GOO
    GOO,
#endif
};
#endif

/// @brief Launch one particle. When the pool is full it takes a random one's place.
/// @param k      Its kind.
/// @param x,y    Where it starts, in pixels.
/// @param vx16   Horizontal speed in 1/16 px per tick (-127..127).
/// @param vy16   Vertical speed in 1/16 px per tick (-127..127; negative is up).
/// @param life   How many ticks it lives.
/// @param colour Its colour.
void spawn(Kind k, int x, int y, int vx16, int vy16, uint8_t life, uint8_t colour);
#if SIZZLE_BURST
/// @brief A radial burst: n particles flying out from a point.
/// @param k       Their kind.
/// @param x,y     The centre.
/// @param n       How many.
/// @param speed16 Top speed in 1/16 px per tick (each gets half to all of it).
/// @param colour  Their colour.
void burst(Kind k, int x, int y, uint8_t n, int speed16, uint8_t colour);  // radial
#endif
#if SIZZLE_KIND_COIN
void fountain(Kind k, int x, int y, uint8_t n);                            // confetti or coins, up
#else
/// @brief A fountain of confetti thrown up from a point (a win).
/// @param x,y Where it springs from.
/// @param n   How many pieces.
void fountain(int x, int y, uint8_t n);                                    // confetti up
#endif
#if SIZZLE_COIN_FLOOR_RUNTIME
void setFloor(int y);                                                      // where coins bounce
#endif
/// @brief Whether any particle is still flying.
/// @return true while one is alive.
bool particles();                                                          // any still flying
/// @brief The same as particles().
/// @return true while a particle is alive.
inline bool particlesAlive() { return particles(); }
#if SIZZLE_HUES_EXPORT
extern const uint8_t SIZZLE_HUES_NAME[5];                                  // the casino rainbow
#endif

// ---------------------------------------------------------------- banner
/// @brief Banner colour styles (the game's SIZZLE_STYLES decide which exist).
enum BannerStyle : uint8_t {
#if SIZZLE_STYLES & SIZZLE_RAINBOW
    B_RAINBOW,      ///< The casino rainbow.
#endif
#if SIZZLE_STYLES & SIZZLE_GOLD
    B_GOLD,         ///< Gold.
#endif
#if SIZZLE_STYLES & SIZZLE_RED
    B_RED,          ///< Red.
#endif
#if SIZZLE_STYLES & SIZZLE_CYAN
    B_CYAN,         ///< Cyan.
#endif
#if SIZZLE_STYLES & SIZZLE_WHITE
    B_WHITE,        ///< White.
#endif
#if SIZZLE_STYLES & SIZZLE_BLACK
    B_BLACK,
#endif
#if SIZZLE_STYLES & SIZZLE_GREEN
    B_GREEN,
#endif
};
/// @brief Show a banner: big centred lettering with an outline that pops in,
///        holds, and blinks out ("BLACKJACK!", "YOU WIN").
/// @param text   The text (copied; at most SIZZLE_BANNER_CHARS - 1 characters).
/// @param s      Its style.
/// @param cy     The row its centre sits on.
/// @param frames How many ticks it stays up, blinking out at the end (default 70).
/// @details One banner at a time: a new one replaces the last.
void banner(const char *text, BannerStyle s, int cy, uint8_t frames = 70);
#if SIZZLE_HOLD_BANNER
/// @brief Keep the banner up (before it blinks out) until let go.
/// @param on true to hold it; false to let it blink out.
void holdBanner(bool on);            // keep the banner up (before it blinks out) until false
#endif
/// @brief Whether a banner is showing.
/// @return true until it has blinked out.
bool bannerActive();

// ---------------------------------------------------------------- floats
#if SIZZLE_FLOATS
/// @brief A floating text ("+$15") that rises from a point and fades out.
/// @param text   The text (copied; at most SIZZLE_FLOAT_CHARS - 1 characters).
/// @param x,y    Where it starts.
/// @param colour Its colour.
/// @details Several can be up at once; a new one takes the first free slot
/// (or the first slot when all four are busy). Each lives 50 ticks.
void floatText(const char *text, int x, int y, uint8_t colour);
/// @brief Draw the floating texts: while drawing, on top of the scene.
void drawFloats();
#endif

/// @brief The band of rows everything transient covers (particles, floats,
///        banner, shake): what a partial redraw must include.
/// @param lo Gets the top row.
/// @param hi Gets the bottom row.
/// @return false if nothing is moving (lo and hi are then meaningless).
bool activeRows(int &lo, int &hi);

/// @brief Remove every particle, float and the banner at once (and stop a shake).
void clear();
/// @brief Move everything on by one tick: once per logic tick.
void update();                      // once per logic tick
#if SIZZLE_DUST == 3
/// @brief Draw the particles: while drawing, on top of the scene.
/// @param dust The size of a DUST puff in pixels (default 2).
void drawParticles(uint8_t dust = 2);   // dust: the size of a DUST puff in pixels
#else
void drawParticles();
#endif
/// @brief Draw the banner, if one is showing: while drawing, last.
void drawBanner();

}  // namespace fx

/// @}
