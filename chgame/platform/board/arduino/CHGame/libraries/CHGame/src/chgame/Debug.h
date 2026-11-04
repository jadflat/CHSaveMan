/// @file Debug.h
/// @brief The serial debug protocol (CHGAME_DEBUG builds only; see chgame/Config.h).
///
/// Line-based ASCII over the USB CDC port, or the simulator's pipe. The
/// simulator, tools/chsim/chdrive.py and tools/device.py run scripts through
/// it, on the PC and on the board alike.
///
/// In a release build every call here is an empty inline, so a sketch calls
/// them without `#if`.
#pragma once
#include <stdint.h>
#include "Config.h"

/// @defgroup chgame_debug Debug protocol
/// @ingroup lib_chgame
/// @brief dbg::: the serial protocol the simulator and the PC tools drive a game through.
///
/// A game calls dbg::begin() at the top of `setup()` and dbg::poll() at the
/// top of `loop()`; the rest is optional. With CHGAME_DEBUG off (a release
/// build) every call is an empty inline that costs nothing.
///
/// | Command | Answer / effect |
/// |---|---|
/// | `?`       | the game's hello line, e.g. `CHCR 0.1` |
/// | `S`       | `FB <frame> 8224` and a newline, then 8192 framebuffer + 32 palette bytes |
/// | `K <hex>` | hold these buttons (ORed with the real ones); `K` alone releases |
/// | `L1` / `L0` | lockstep on / off (on: the game only advances on `N`) |
/// | `N <k>`   | run k frames, then answer `OK <frame>` |
/// | `P`       | `PERF rnd=<us> max=<us> late=<n> frames=<n> stk=<b>` (+ `fstk=<b>` with a second frame stack, + host ns in the sim) |
/// | `T`       | `PROF <slot>=<us> ...` section timings (CHGAME_PROFILE) |
/// | `B`       | reboot into the CHGame bootloader |
/// | `!`       | `FAULT none`, or the crash before the last restart (the core's fault handler: A restarts after one) as `FAULT mcause=<hex> mepc=<hex> mtval=<hex> ra=<hex> sp=<hex>` |
/// | `Q`       | (simulator) `CAL <ns> x5`: host time of CHGfx's benchmark primitives, for estimated device times (chdrive's cal) |
/// | anything else | goes to the game's hook: `OK` if it took it, else `ERR` |
/// @{

/// @brief The serial debug protocol.
namespace dbg {

#if CHGAME_DEBUG
/// @brief Start the protocol: at the top of `setup()`.
/// @param hello The line `?` answers: the game's id and version (e.g.
///              "CHCR 0.1"), so a tool can check it is talking to the right
///              game. Each game needs its own id.
/// @details Also paints the stack for its high-water mark (P's stk=) and
/// picks up the record of a crash before the last restart (`!`).
void begin(const char *hello);
/// @brief Read and answer commands: at the top of `loop()`, before nextFrame().
void poll();                        // at the top of loop(): serial input
/// @brief Frame timing for `P`: call when the logic tick starts.
void markUpdateStart();
/// @brief Frame timing for `P`: call when drawing starts.
void markRenderStart();
/// @brief Frame timing for `P`: call when drawing ends (before the flush).
void markRenderEnd();
/// @brief Send text to the host as it is (no newline added).
/// @param s The text.
void print(const char *s);
/// @brief In the simulator, block until input may have arrived (elsewhere nothing).
void waitInput();                   // the simulator: block until input may have arrived
/// @brief Parse a number from a command's arguments, for a game's hook.
/// @param p    The text; skips leading spaces and commas, and is left after the number.
/// @param base 10 or 16.
/// @return The number (0 if there are no digits).
uint32_t parseNum(const char *&p, uint8_t base);   // skips leading spaces/commas
/// @brief The game's own commands, or nullptr.
/// @details Called with the command's first character and the rest of the
/// line; returns true if it handled the command (the host gets "OK"), false
/// if not ("ERR").
extern bool (*hook)(char cmd, const char *args);
/// @brief The longest command line, in characters.
static const uint8_t LINE = 100;    // the longest command line
/// @brief Hold the game's commands while it is busy, in a caller's buffer.
/// @details A game busy with something long (a search) can hold its commands:
/// while busy(cmd) is true a command waits (answered HELD at once, OK/ERR
/// once it has run) while N, S, P and the rest go on working.
/// @param busy Returns true while a command must wait.
/// @param buf  LINE bytes for the waiting command.
void holdInto(bool (*busy)(char cmd), char *buf);
/// @brief holdInto() with a buffer of its own (which exists only in a game that calls this).
/// @param busy Returns true while a command must wait.
inline void holdWhile(bool (*busy)(char cmd)) { static char held[LINE]; holdInto(busy, held); }
/// @brief Report a second stack's high-water mark in `P` (fstk=): frames drawn
///        from inside a search, for instance.
/// @param lo,hi The stack's bounds (it is painted now).
void frameStack(uint32_t *lo, uint32_t *hi);
#if CHGAME_PROFILE
/// @brief Start a profiled frame (CHGAME_PROFILE builds): `T` reports averages per call.
/// @details prof(i) charges the time since the previous prof() to slot i
/// (0-11); T reports and resets the averages per profStart().
void profStart();
/// @brief Charge the time since the previous prof() to a slot.
/// @param slot 0-11.
void prof(uint8_t slot);
#else
inline void profStart() {}
inline void prof(uint8_t) {}
#endif
#else
inline void begin(const char *) {}
inline void poll() {}
inline void markUpdateStart() {}
inline void markRenderStart() {}
inline void markRenderEnd() {}
inline void print(const char *) {}
inline void waitInput() {}
inline void holdWhile(bool (*)(char)) {}
inline void frameStack(uint32_t *, uint32_t *) {}
inline void profStart() {}
inline void prof(uint8_t) {}
#endif

}  // namespace dbg

/// @}
