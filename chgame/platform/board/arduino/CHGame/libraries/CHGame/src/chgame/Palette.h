/// @file Palette.h
/// @brief The house palette: sixteen named colours, and the tricks a palette allows.
///
/// CHGfx draws with palette indices (0-15) and applies the palette while it
/// converts the framebuffer for the panel, so recolouring an index recolours
/// every pixel that uses it, for free. pal:: keeps a staging copy in RGB444
/// (the 12 bpp panel's own grid, so output, fades and the simulator agree)
/// and pal::commit() works out the final colours once a frame: the felt
/// theme, FX_A/FX_B colour cycling, a flash, desaturation and the fade.
/// CHGfx stages gfx_setPalette() itself (it takes effect when the next flush
/// starts), so commit() may run while a frame is still going out; it only
/// calls it when a colour really changed, because each change costs CHGfx a
/// lookup-table rebuild.
///
/// Every frame: pal::tick() once per logic tick, pal::commit() before the
/// flush.
#pragma once
#include <stdint.h>

/// @defgroup chgame_palette Palette
/// @ingroup lib_chgame
/// @brief The sixteen house colours and pal::: felt themes, fades, flashes
///        and colour cycling.
///
/// Draw with the colour names (`gfx_clear(FELT)`, `text35(x, y, s, GOLD)`).
/// Set the palette up once with pal::init(), then every frame:
/// @code
/// pal::tick();          // once per logic tick: FX_A/FX_B cycle, flashes count down
/// gfx_wait();
/// pal::commit();        // before the flush: hands the colours to CHGfx if they changed
/// // ... draw ...
/// gfx_flushAsync();
/// @endcode
/// Colours are RGB444 (`0xRGB`, 4 bits a channel), the 12 bpp panel's own grid.
/// @{

/// @brief The house colours, by palette index.
/// @details A game that needs other colours in a slot passes its own table to
/// pal::init() and names the slot itself (CHDominoes has BONE and SLATE where
/// SKIN and CYAN are).
enum : uint8_t {
    INK = 0,    ///< 0: black (0x000), outlines and text.
    WHITE,      ///< 1: white (0xFFF).
    FELT_DK,    ///< 2: dark felt; re-dyed by pal::setTheme().
    FELT,       ///< 3: the table felt; re-dyed by pal::setTheme().
    FELT_LT,    ///< 4: light felt; re-dyed by pal::setTheme().
    SILVER,     ///< 5: light grey (0xBBC).
    RED,        ///< 6: red (0xE12).
    WINE,       ///< 7: dark red (0x702).
    GOLD,       ///< 8: gold (0xFC2).
    WOOD,       ///< 9: brown (0x741), table rails.
    BLUE,       ///< 10: blue (0x26E).
    NAVY,       ///< 11: dark blue (0x125).
    SKIN,       ///< 12: skin tone (0xFB8).
    CYAN,       ///< 13: cyan (0x6EF).
    FX_A,       ///< 14: animated by pal::tick() while cycling is on (see pal::Mode).
    FX_B,       ///< 15: animated by pal::tick() while cycling is on (see pal::Mode).
};

/// @brief The palette: themes, fades, flashes and colour cycling.
namespace pal {

/// @brief The house colours above, RGB444, in index order. pal::init()'s default.
extern const uint16_t HOUSE[16];

/// @brief The felt colours: FELT_DK, FELT and FELT_LT can be re-dyed as a whole.
enum Theme : uint8_t {
    GREEN,          ///< The classic green felt (the default).
    BLUE_FELT,      ///< Blue felt.
    RED_FELT,       ///< Red felt.
    PURPLE,         ///< Purple felt.
    THEME_COUNT     ///< The number of built-in themes.
};
/// @brief The built-in felts: FELT_DK, FELT, FELT_LT in RGB444 for each Theme.
extern const uint16_t FELTS[THEME_COUNT][3];

/// @brief What FX_A and FX_B do by themselves while cycling is on (setMode()).
enum Mode : uint8_t {
    /// FX_A runs through a rainbow (banner outlines, sparkle); FX_B shimmers
    /// gold - white - gold. The default.
    CASINO,
    /// FX_A shimmers cyan - white (squares a piece can go to); FX_B pulses
    /// red - gold (pieces it can take).
    TARGETS,
    /// FX_A fades black - white - black about once a second (a cursor
    /// outline); FX_B as CASINO.
    HOVER,
    /// FX_A flickers red - gold - white (a hot hand); FX_B as CASINO.
    FIRE
};

/// @brief Load a set of colours and commit them at once.
/// @param base Sixteen RGB444 colours, in index order (default: HOUSE, the
///             green felt). FELT_DK..FELT_LT come from here until setTheme().
/// @details Call once in `setup()`, after gfx_begin().
void init(const uint16_t *base = HOUSE);
/// @brief Replace the built-in felt themes with a game's own.
/// @param felts  A table of `count` themes, each FELT_DK, FELT, FELT_LT in RGB444
///               (kept by pointer: it must stay valid).
/// @param count  The number of themes in the table.
void setThemes(const uint16_t (*felts)[3], uint8_t count);
/// @brief Re-dye the felt (FELT_DK, FELT, FELT_LT).
/// @param theme A Theme, or an index into the table given to setThemes(); an
///              index past the end selects theme 0.
void setTheme(uint8_t theme);
/// @brief The current felt theme.
/// @return The index last given to setTheme() (0 at start).
uint8_t theme();
/// @brief Choose what FX_A and FX_B animate as.
/// @param mode A Mode (default CASINO).
void setMode(uint8_t mode);
/// @brief Fade the whole screen toward black.
/// @param level 0 = black .. 16 = full colour (the default); larger values are 16.
/// @details Fade in or out by stepping the level a frame at a time.
void setFade(uint8_t level);
/// @brief The current fade level.
/// @return 0 (black) .. 16 (full colour).
uint8_t fade();
/// @brief Wash the colours toward grey (a paused game, a game over).
/// @param amount 0 = full colour (the default) .. 16 = grey.
void setDesaturate(uint8_t amount);
/// @brief Show one palette index in another colour for a few frames (a hit, a win).
/// @param index  The palette index to flash (0-15).
/// @param rgb444 The colour to show it in.
/// @param frames How many tick()s the flash lasts. One flash at a time: a new
///               one replaces the last.
void flash(uint8_t index, uint16_t rgb444, uint8_t frames);
/// @brief Set a colour by hand.
/// @param index  The palette index (0-15); for FX_A/FX_B, turn cycling off
///               first or tick() overwrites it.
/// @param rgb444 The colour.
void setFx(uint8_t index, uint16_t rgb444);
/// @brief Whether FX_A/FX_B animate by themselves.
/// @param on true (the default) to cycle as setMode() says; false to leave them
///           as set with setFx().
void setCycling(bool on);
/// @brief Advance the colour cycling and any flash by one tick.
/// @details Call once per logic tick, before commit().
void tick();
/// @brief Restart the FX_A/FX_B cycle from its beginning.
/// @details The debug protocol uses it so scripted runs repeat exactly.
void resetClock();
/// @brief Work out the final colours and hand them to CHGfx if any changed.
/// @details Call once per frame, before the flush (after gfx_wait() is the
/// usual place, but any time is safe: CHGfx stages the palette for the next
/// flush). A frame where nothing changed costs a few comparisons.
void commit();
/// @brief A staged colour, before fade, flash and desaturation.
/// @param index The palette index (0-15).
/// @return Its colour, RGB444.
uint16_t rgb444(uint8_t index);

}  // namespace pal

/// @}
