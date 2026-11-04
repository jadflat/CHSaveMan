/// @file Audio.h
/// @brief Sound and the status LED: a small sequencer for the piezo.
///
/// One pin plays one note at a time, driven by TIM1 channel 2 on PB10 and
/// stepped by the core's 1 kHz SysTick hook, so no other timer is used. It
/// plays two kinds of thing at once:
///
///   - **effects**: short step lists (a pitch, the pitch it sweeps to, a
///     length), each with a priority: a new effect is refused while one of
///     higher priority sounds, and cuts off any other;
///   - **music**, under the effects: a Playtune score (several voices;
///     tools/make_music.py writes them), rendered as a lead line or an
///     arpeggio, or a melody (one line of notes, easy to write by hand).
///
/// The simulator is silent: the same interface with no hardware, which
/// remembers the last effect asked for (audio::simLast) for scripts and
/// tests. The game's tools/audio preview renders the real thing to WAV.
#pragma once
#include <stdint.h>

/// @defgroup chgame_audio Sound and LED
/// @ingroup lib_chgame
/// @brief audio::: sound effects, music and the status LED.
///
/// A game lists its effects once, in the order of its own enum:
/// @code
/// enum class Sfx : uint8_t { Cursor, Win, COUNT };
/// AUDIO_STEPS(CURSOR) = { AUDIO_STEP(2100, 0, 10) };
/// AUDIO_STEPS(WIN)    = { AUDIO_STEP(2093, 0, 60), AUDIO_STEP(4186, 0, 170) };
/// const audio::Effect SOUNDS[] = { AUDIO_EFFECT(CURSOR, 0), AUDIO_EFFECT(WIN, 3) };
///
/// audio::begin(SOUNDS, (uint8_t)Sfx::COUNT);   // in setup()
/// audio::sfx(Sfx::Win);                        // any time
/// audio::update();                             // once per logic tick (the LED)
/// @endcode
/// @{

/// @brief Sound effects, music and the status LED.
namespace audio {

/// @brief An effect step in three bytes. Write them with AUDIO_STEP() and AUDIO_REST().
/// @details 20 Hz is under half a percent of a piezo's 1-4 kHz: no ear hears
/// it, and the tables are half the size. A step lasts at most 510 ms.
struct Step {
    uint8_t hz,     ///< The pitch in 20 Hz units; 0 = a rest.
            endHz,  ///< The pitch it sweeps to, in 20 Hz units; 0 = none (holds hz).
            ms;     ///< The length in 2 ms units.
};
/// @brief An effect step: a pitch, the pitch it sweeps to, a length.
/// @param hz  Start pitch in Hz (rounded to 20 Hz).
/// @param end The pitch to sweep to in Hz, or 0 to hold hz.
/// @param ms  Length in milliseconds (rounded to 2 ms; at most 510).
#define AUDIO_STEP(hz, end, ms) \
    { (uint8_t)(((hz) + 10) / 20), (uint8_t)(((end) + 10) / 20), (uint8_t)(((ms) + 1) / 2) }
/// @brief A silent effect step.
/// @param ms Length in milliseconds (rounded to 2 ms; at most 510).
#define AUDIO_REST(ms) { 0, 0, (uint8_t)(((ms) + 1) / 2) }
/// @brief Declare an effect's step table, kept in flash.
/// @param name The table's name, for AUDIO_EFFECT().
/// @details Use it as `AUDIO_STEPS(WIN) = { AUDIO_STEP(...), ... };`. (A plain
/// `static const` table of 8 bytes or less is "small data" to the compiler,
/// and this core's link script copies small data into SRAM.)
#if defined(__riscv) && !defined(CHSIM)
#define AUDIO_STEPS(name) static const audio::Step name[] __attribute__((section(".rodata.audio." #name)))
#else
#define AUDIO_STEPS(name) static const audio::Step name[]
#endif

/// @brief Flags ORed into an effect's priority (0-15).
enum : uint8_t {
    SOFT  = 0x10,       ///< A narrow pulse: quieter than everything else (clocks, ticks).
    GLIDE = 0x20,       ///< Sweeps change pitch at a cycle's end instead of restarting it.
};
/// @brief An effect: its steps, and its priority with flags. Write them with AUDIO_EFFECT().
struct Effect {
    const Step *steps;  ///< The step table (AUDIO_STEPS()).
    uint8_t n,          ///< Number of steps.
            flags;      ///< Priority 0-15, ORed with SOFT and GLIDE.
};
/// @brief An entry of the effects table.
/// @param steps A table declared with AUDIO_STEPS().
/// @param flags The priority (0-15), ORed with SOFT and GLIDE if wanted.
#define AUDIO_EFFECT(steps, flags) { steps, (uint8_t)(sizeof(steps) / sizeof((steps)[0])), (uint8_t)(flags) }

/// @brief Set up the speaker, the LED pin and the timer, with the game's effects.
/// @param effects The effects table, in the order of the game's enum (kept by pointer).
/// @param count   The number of effects.
/// @param on      Whether sound starts on (a saved option, for instance).
void begin(const Effect *effects, uint8_t count, bool on = true);
/// @brief Turn sound on or off.
/// @param on false: silent, effects refused, music stopped in place (it
///           carries on from there when sound comes back).
void setOn(bool on);                // off: silent, effects refused, music stopped in place
/// @brief Whether sound is on.
/// @return true after begin() with sound on, until setOn(false).
bool on();

/// @brief Play an effect.
/// @param id An index into the effects table, or the game's enum.
/// @details Refused while an effect of higher priority sounds; cuts off any
/// other effect.
void sfx(uint8_t id);
/// @brief Play an effect raised in pitch: a combo that climbs.
/// @param id        An index into the effects table.
/// @param semitones How far to raise the whole effect, 0-12.
void sfx(uint8_t id, uint8_t semitones);
/// @brief Play an effect named by the game's enum.
/// @param e The enum value (its index into the effects table).
template <class E> inline void sfx(E e) { sfx((uint8_t)e); }
/// @brief Play an effect named by the game's enum, raised in pitch.
/// @param e         The enum value.
/// @param semitones How far to raise it, 0-12.
template <class E> inline void sfx(E e, uint8_t semitones) { sfx((uint8_t)e, semitones); }
/// @brief One note made up on the spot (a typewriter, a click, a counter rolling).
/// @details It cuts off effects of priority 0 and 1, is refused over anything
/// higher, and is itself priority 0. To never play over any effect:
/// `if (!audio::playing()) audio::blip(...)`.
/// @param hz   Pitch in Hz.
/// @param ms   Length in milliseconds.
/// @param soft true for the quieter narrow pulse (as SOFT).
void blip(uint16_t hz, uint16_t ms, bool soft = false);
/// @brief One note at a priority, like an effect.
/// @param hz       Pitch in Hz.
/// @param ms       Length in milliseconds.
/// @param priority 0-15, as an effect's.
void note(uint16_t hz, uint16_t ms, uint8_t priority);
/// @brief Whether an effect is sounding.
/// @return true while an effect, blip() or note() plays.
bool playing();                     // an effect is sounding

/// @brief How music is rendered on the one pin.
/// @details The music code is linked in only by a game that calls music() or melody().
enum MusicMode : uint8_t {
    MUSIC_OFF,      ///< No music (held where it is).
    ARPEGGIO,       ///< A score's sounding voices take 6 ms turns (the default).
    LEAD            ///< The melody (channel 0) plays; the others fill only its real rests.
};
/// @brief Choose how music is rendered.
/// @param mode A MusicMode; MUSIC_OFF holds the tune where it is.
void setMusic(uint8_t mode);        // default ARPEGGIO; MUSIC_OFF holds the tune where it is
/// @brief Play a Playtune score under the effects.
/// @param score The score (from tools/make_music.py), or nullptr to stop.
/// @param loop  true (default): start again at the end.
void music(const uint8_t *score, bool loop = true);   // nullptr: stop

/// @brief A melody: one line of notes, easy to write by hand.
/// @details (MIDI note, length) pairs: note 0 is a rest, a length counts
/// units of unitMs, and each note is let go gapMs early so repeated notes
/// stay apart. Middle C is 60; the piezo sings best from C6 (84) to C8 (108).
/// @code
/// static const uint8_t TUNE[] = { 84,2, 88,2, 91,4, 0,2 };
/// static const audio::Melody TITLE = { TUNE, sizeof TUNE / 2, 105, 14 };
/// audio::melody(TITLE);
/// @endcode
struct Melody {
    const uint8_t *notes;   ///< (MIDI note, length) pairs.
    uint8_t count,          ///< Number of pairs.
            unitMs,         ///< Milliseconds per length unit.
            gapMs;          ///< Silence at the end of each note, in milliseconds.
};
/// @brief Play a melody under the effects (it replaces any score).
/// @param m    The melody (kept by reference: it must stay valid).
/// @param loop true (default): start again at the end.
void melody(const Melody &m, bool loop = true);       // replaces any score
/// @brief Stop the music.
void stopMusic();
/// @brief Whether the music repeats.
/// @param on false: the score stops at its end instead of repeating.
void loopMusic(bool on);            // off: the score stops at its end instead of repeating
/// @brief Whether music is playing.
/// @return true while a score or melody plays; false once it has ended, and
///         while sound or music is off.
bool musicPlaying();                // false while sound or music is off

/// @brief Run the LED patterns: once per logic tick.
void update();                      // once per logic tick: the LED patterns

/// @brief Status LED (PB9) patterns, for wins. Timed in update() ticks.
enum Led : uint8_t {
    LED_OFF,        ///< Off.
    LED_BLINK,      ///< One blink (12 ticks).
    LED_TRIPLE,     ///< Three blinks.
    LED_PARTY       ///< Fast blinking for about 4 seconds at 60 fps.
};
/// @brief Start an LED pattern (replacing the one running).
/// @param pattern The pattern.
void led(Led pattern);

#ifdef CHSIM
extern uint8_t simLast;             // the last effect played (0xFF: none yet)
#endif

}  // namespace audio

/// @}
