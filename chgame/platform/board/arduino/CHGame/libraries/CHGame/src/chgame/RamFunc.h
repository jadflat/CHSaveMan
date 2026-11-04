/// @file RamFunc.h
/// @brief RAMFUNC(name): run a function from SRAM.
///
/// The section name is CHGfx 1.3's trick: the core's link script puts
/// *(.gnu.linkonce.r.*) first in .data (copied to SRAM at boot), ahead of
/// .sdata/.sbss. linkonce merges sections of the same name, so every
/// function needs a name of its own (which also lets the linker drop unused
/// ones one at a time). The library's are "chg.<name>"; CHGfx's are
/// "chgfx.<name>". A sketch's own, RAMFUNC(name), are "app.<name>", so a
/// sketch's function can never merge with one of the library's of the same
/// name (but two of the sketch's must have different names).
///
/// The simulator keeps noinline but not the section: Mach-O (macOS) rejects
/// ELF section names.
#pragma once

/// @defgroup chgame_ramfunc RAM functions
/// @ingroup lib_chgame
/// @brief RAMFUNC(name): put a hot function in SRAM, about three times faster than flash.
///
/// Flash has 3 wait states and no cache: from flash a per-pixel loop costs
/// ~3 us a pixel on this part; from SRAM about a third of that. Every
/// RAMFUNC costs its size in RAM, so keep it for the inner loops.
/// @code
/// RAMFUNC(blitRow) static void blitRow(uint8_t *dst, ...) { ... }
/// @endcode
/// @{

#if defined(__riscv) && !defined(CHSIM)
/// @brief The library's own SRAM functions (section "chg.<name>").
/// @param name A name unique among the library's RAM functions.
#define CHGAME_RAMFUNC(name) __attribute__((section(".gnu.linkonce.r.chg." #name), noinline))
/// @brief A sketch's SRAM functions (section "app.<name>"); RAMFUNC() is this.
/// @param name A name unique within the sketch.
#define CHGAME_APP_RAMFUNC(name) __attribute__((section(".gnu.linkonce.r.app." #name), noinline))
#else
#define CHGAME_RAMFUNC(name) __attribute__((noinline))     // the simulator, host tests and tools
#define CHGAME_APP_RAMFUNC(name) __attribute__((noinline))
#endif

#ifndef RAMFUNC
/// @brief Run this function from SRAM. Put it before the function's declaration.
/// @param name A name unique within the sketch (any identifier; the
///             function's own name is the usual choice).
#define RAMFUNC(name) CHGAME_APP_RAMFUNC(name)
#endif

/// @}
