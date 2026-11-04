/// @file Fx.h
/// @brief Motion maths and the screen shake: easing, integer sine, randomness.
///
/// Integer maths only: soft-float trig once cost a demo on this chip 8.5 KB
/// of flash and half its frame rate.
///
/// A game's own effects (its particle kinds, banners, floating texts) live in
/// the same namespace in the game's code, built from these (see chgame/Sizzle.h).
#pragma once
#include <stdint.h>

/// @defgroup chgame_fx Effects maths
/// @ingroup lib_chgame
/// @brief fx::: easing curves, integer sine and cosine, presentation
///        randomness and the screen shake.
///
/// @code
/// int y = 20 + fx::ease(fx::OUT_BACK, t, 30) * 60 / 256;   // slide in over 30 frames
/// int bob = fx::isin(chgame.frameCount * 4) * 3 / 256;      // +-3 px, a turn every 64 frames
/// @endcode
/// @{

/// @brief Effects: motion maths, the screen shake, and (chgame/Sizzle.h) particles,
///        banners and floating texts.
namespace fx {

/// @brief Easing curves for ease().
/// @details Each curve is a 17-point table; ease() is inline, so a call with a
/// constant curve links only that curve's table.
enum Ease : uint8_t {
    LINEAR,         ///< Straight.
    OUT_CUBIC,      ///< Fast, then slowing to a stop.
    OUT_BACK,       ///< Overshoots the end, then settles back (goes past 256).
    IN_OUT,         ///< Slow, fast, slow.
    OUT_BOUNCE      ///< Drops and bounces to a stop (overshoots).
};
/// @brief The curves' tables (17 points, 0..256), for easeCurve().
extern const int16_t EASE_CUBIC[17], EASE_BACK[17], EASE_INOUT[17], EASE_BOUNCE[17];
/// @brief Ease through a curve table.
/// @param curve A 17-point table (EASE_CUBIC ...), or nullptr for linear.
/// @param t     Time, 0..n.
/// @param n     Duration.
/// @return 0 at t <= 0, 256 at t >= n, the curve in between (interpolated).
int easeCurve(const int16_t *curve, int t, int n);     // curve nullptr: linear
/// @brief Ease: progress through a curve.
/// @param e Which curve.
/// @param t Time, 0..n (clamped).
/// @param n Duration (frames, for instance).
/// @return 0..256 (OUT_BACK and OUT_BOUNCE overshoot 256 on the way).
inline int ease(Ease e, int t, int n) {
    return easeCurve(e == OUT_CUBIC ? EASE_CUBIC : e == OUT_BACK ? EASE_BACK : e == IN_OUT ? EASE_INOUT
                     : e == OUT_BOUNCE ? EASE_BOUNCE : nullptr, t, n);
}
/// @brief The OUT_BOUNCE curve.
/// @param t Time, 0..n.
/// @param n Duration.
/// @return 0..256 (with the bounces).
inline int bounce(int t, int n) { return easeCurve(EASE_BOUNCE, t, n); }
/// @brief Integer sine.
/// @param a Angle in 1/256 turns (any value: it wraps).
/// @return -256..256.
int isin(int a);                    // a in 1/256 turns -> -256..256
/// @brief Integer cosine.
/// @param a Angle in 1/256 turns.
/// @return -256..256.
inline int icos(int a) { return isin(a + 64); }

/// @brief A random number for presentation (xorshift32).
/// @details Presentation-only randomness: sparkle, confetti, shake. It never
/// touches game outcomes (those use the game's own generator), and reseed()
/// restarts it so scripted runs repeat.
/// @return 32 random bits.
uint32_t rnd();
/// @brief A random number in a range, for presentation.
/// @param lo The smallest value.
/// @param hi One past the largest value.
/// @return lo .. hi-1 (lo if hi <= lo).
int rndRange(int lo, int hi);       // lo .. hi-1
/// @brief Restart rnd() from its fixed seed.
void reseed();

/// @brief Start a screen shake.
/// @details shake() starts it; applyShake() moves the finished frame and
/// shakeTick() counts it down once per logic tick.
/// @param frames    How many ticks it lasts.
/// @param amplitude The vertical jolt in pixels at 10 ticks left; it scales
///                  with the ticks left, so the shake dies away (at least 1 px).
///                  It also moves 2 px sideways.
void shake(uint8_t frames, uint8_t amplitude);
/// @brief Whether a shake is running.
/// @return true until it has counted down.
bool shaking();
/// @brief Shake the finished framebuffer: call after drawing, before the flush.
/// @param y0,y1 The band of rows to move (inclusive).
/// @param fill  < 0 (default): rows the move uncovers keep their pixels,
///              shifted in place; else they are filled with this colour.
void applyShake(int y0, int y1, int fill = -1);
/// @brief Count the shake down one tick: once per logic tick.
void shakeTick();
/// @brief Stop the shake at once.
void shakeStop();

}  // namespace fx

/// @}
