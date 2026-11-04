/// @file Input.h
/// @brief Buttons and frame pacing: the Arduboy-flavoured front of the CHGame board.
///
/// Started from the helper shared by CHSpriteView/CHMultiSprite/CHStlView and
/// reworked for the casino games, which all used this copy:
///   - button masks are parenthesised, so ~UP_BUTTON and A|B behave;
///   - every query reads the state captured by pollButtons(), so a frame sees
///     one consistent snapshot and injected input (debug protocol, simulator)
///     behaves exactly like a real press;
///   - nextFrame() uses a microsecond accumulator, so 60 fps is 60.0, not the
///     62.5 that 1000/60 = 16 ms gave;
///   - a lockstep mode lets the debug protocol step the game frame by frame;
///   - auto-repeat for held buttons (menus, bet adjust);
///   - holding START for 3 s leaves for the SD game menu (startExits).
#pragma once
#include <stdint.h>

/// @defgroup chgame_input Buttons and frame pacing
/// @ingroup lib_chgame
/// @brief The global @ref ::chgame object: the frame clock and the eight buttons.
///
/// Call chgame.boot() once in `setup()`, then in `loop()`:
/// @code
/// if (!chgame.nextFrame()) return;     // not time for a frame yet
/// chgame.pollButtons();                // one snapshot of the buttons for this frame
/// if (chgame.justPressed(A_BUTTON)) { ... }
/// if (chgame.pressed(LEFT_BUTTON | B_BUTTON)) { ... }   // both held
/// @endcode
///
/// The button names and the calls are the Arduboy2 library's, so code
/// written for it reads the same here.
/// @{

#define A_BUTTON      (1u << 0)     ///< The A button (PB1).
#define B_BUTTON      (1u << 1)     ///< The B button (PB6).
#define UP_BUTTON     (1u << 2)     ///< D-pad up (PB4).
#define DOWN_BUTTON   (1u << 3)     ///< D-pad down (PC14).
#define LEFT_BUTTON   (1u << 4)     ///< D-pad left (PB3).
#define RIGHT_BUTTON  (1u << 5)     ///< D-pad right (PC15).
#define START_BUTTON  (1u << 6)     ///< START (PB8). Held 3 s, it leaves for the menu (CHGame::startExits).
#define SELECT_BUTTON (1u << 7)     ///< SELECT (PB7).

/// @brief Read the buttons straight from the hardware.
/// @return A mask of the buttons held right now (pressed = 1), `A_BUTTON` ...
///         `SELECT_BUTTON` ORed together.
/// @details Implemented per platform (device: GPIO registers; simulator: the
/// scripted input). A game normally uses CHGame::pollButtons() and the
/// queries instead, which also see injected input.
uint8_t chgame_readButtons();

/// @brief Leave the game for the bootloader's game menu. Never returns.
/// @details A plain reset, with no boot request
/// (platform/bootloader/shared/chgame_bootreq.h). Under a bootloader without
/// the menu (0.2.4) the game simply starts again. In the simulator it prints
/// a line and ends the run.
[[noreturn]] void chgame_exitToMenu();

/// @brief The board's buttons and frame clock. There is one, @ref ::chgame.
class CHGame {
public:
    /// @brief Set up the button pins (inputs with pull-ups) and take a first reading.
    /// @details Call once at the top of `setup()`. A button held at boot does
    /// not count as a press on the first frame.
    void boot();
    /// @brief Set the frame rate nextFrame() paces to.
    /// @param fps Frames per second (1-255). The default is 60.
    void setFrameRate(uint8_t fps);
    /// @brief Whether it is time for the next frame.
    /// @return true once per frame period (and counts the frame in frameCount);
    ///         false otherwise, so `loop()` returns at once.
    /// @details A game that falls more than three frames behind (a debug
    /// pause, a save) starts afresh instead of sprinting to catch up. In
    /// lockstep (the debug protocol's `L1`) it returns true only for the
    /// frames the tool asked for.
    bool nextFrame();

    /// @brief Take this frame's snapshot of the buttons.
    /// @details Call once per frame, after nextFrame(). Every query below
    /// reads this snapshot, so a frame sees one consistent state. It ORs in
    /// `injected` (the debug protocol's `K`), counts how long each button has
    /// been held (for repeat()), and calls exitToMenu() when START has been
    /// held 3 s and startExits is set.
    void pollButtons();
    /// @brief The buttons held in this frame's snapshot.
    /// @return A mask of `A_BUTTON` ... `SELECT_BUTTON`.
    uint8_t buttons() const              { return cur; }
    /// @brief Whether all the buttons in a mask are held.
    /// @param b One button or several ORed together.
    /// @return true if every one of them is held.
    bool pressed(uint8_t b) const        { return (cur & b) == b; }
    /// @brief Whether any of the buttons in a mask is held.
    /// @param b One button or several ORed together.
    /// @return true if at least one of them is held.
    bool anyPressed(uint8_t b) const     { return (cur & b) != 0; }
    /// @brief Whether a button went down this frame.
    /// @param b One button or several ORed together.
    /// @return true if any of them is held now and was not in the previous frame.
    bool justPressed(uint8_t b) const    { return (cur & ~prev & b) != 0; }
    /// @brief Whether a button was let go this frame.
    /// @param b One button or several ORed together.
    /// @return true if any of them was held in the previous frame and is not now.
    bool justReleased(uint8_t b) const   { return (prev & ~cur & b) != 0; }
    /// @brief Every button that went down this frame.
    /// @return A mask of the buttons pressed now and not in the previous frame.
    uint8_t justPressedMask() const      { return (uint8_t)(cur & ~prev); }
    /// @brief Press edge, then auto-repeat while held: for menus and adjusting a bet.
    /// @param b     One button or several ORed together.
    /// @param delay Frames held before the repeat starts (default 18, 0.3 s at 60 fps).
    /// @param rate  Frames between repeats after that (default 5).
    /// @return true on the frame a button went down, then every `rate` frames
    ///         once it has been held longer than `delay`.
    bool repeat(uint8_t b, uint8_t delay = 18, uint8_t rate = 5) const;
    /// @brief Forget this frame's presses: justPressed() and justReleased() are
    ///        false until the buttons change again.
    /// @details Use it when a screen changes, so the press that changed it
    /// does not also act on the new one.
    void clearButtonState()              { prev = cur; }

    /// @brief Whether frameCount is a multiple of n: something to do every n frames.
    /// @param n The period in frames (not 0).
    /// @return true on every nth frame.
    bool everyXFrames(uint16_t n) const  { return (frameCount % n) == 0; }

    /// @brief Leave the game for the SD game menu. Never returns.
    /// @see chgame_exitToMenu()
    [[noreturn]] void exitToMenu()       { chgame_exitToMenu(); }

    uint32_t frameCount = 0;        ///< Frames since boot, counted by nextFrame().
    uint8_t  injected = 0;          ///< Buttons ORed into the physical ones (the debug protocol's `K`).
    /// START held 3 s calls exitToMenu(); a game that needs a long START hold
    /// clears it in setup().
    bool     startExits = true;
#ifdef CHSIM
    int32_t  lockstep = 0;          // the simulator starts paused, driven by N
#else
    /// Lockstep: < 0 free-running, else the frames nextFrame() may still run
    /// (set by the debug protocol's `L` and `N`).
    int32_t  lockstep = -1;
#endif

private:
    uint8_t  cur = 0, prev = 0;
    uint16_t held[8] = {};          // frames each button has been held
    uint32_t period = 16667, next = 0;
};

/// @brief The one instance: chgame.boot(), chgame.pressed() ...
extern CHGame chgame;

/// @}
