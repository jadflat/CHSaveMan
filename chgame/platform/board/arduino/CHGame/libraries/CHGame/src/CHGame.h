/// @file CHGame.h
/// @brief The one include for a CHGame sketch.
///
/// @code
/// #include <CHGame.h>
/// @endcode
///
/// brings, on top of CHGfx (the framebuffer, the panel and its DMA):
///
/// | Header | What it gives |
/// |---|---|
/// | chgame/Input.h   | CHGame `chgame`: buttons, frame pacing, START held 3 s goes back to the SD game menu |
/// | chgame/Palette.h | the house colours (INK, WHITE, FELT ... FX_A, FX_B) and pal:: (themes, fades, flashes, colour cycling) |
/// | chgame/Draw.h    | rounded panels, span sprites, dithering, the 3x5 font |
/// | chgame/Mask.h    | outlined, shadowed and gradient lettering and logos |
/// | chgame/Fx.h      | fx:: easing, integer sine, randomness, screen shake |
/// | chgame/Audio.h   | audio:: effects, music and the status LED (the piezo) |
/// | chgame/Save.h    | save:: a record in flash that survives power cycles |
/// | chgame/Fmt.h     | number formatting without printf |
/// | chgame/RamFunc.h | RAMFUNC: code that runs from SRAM |
/// | chgame/Debug.h   | dbg:: the serial debug protocol the simulator and the tools drive a game through (CHGAME_DEBUG builds) |
/// | chgame/Config.h  | the library's build switches (CHGAME_DEBUG ...) |
///
/// The casino games in this library's examples are built from these. Only
/// what a sketch calls is linked: CHGfx's framebuffer and DMA handler come
/// with the first drawing call, not with the include, so a sketch that uses
/// this library for its sound, buttons or saves alone pays nothing for them.
/// chgame/Sizzle.h (particles, banners, floating texts) is not included
/// here: a game configures and compiles it itself.
#pragma once
#include <CHGfx.h>
#include "chgame/Config.h"
#include "chgame/Input.h"
#include "chgame/RamFunc.h"
#include "chgame/Palette.h"
#include "chgame/Draw.h"
#include "chgame/Mask.h"
#include "chgame/Fx.h"
#include "chgame/Audio.h"
#include "chgame/Fmt.h"
#include "chgame/Save.h"
#include "chgame/Debug.h"
