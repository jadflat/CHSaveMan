/// @file Config.h
/// @brief The CHGame library's build switches.
///
/// The library is compiled on its own, so it cannot see a sketch's config.h:
/// set these with build.extra_flags (chgame build --debug passes
/// -DCHGAME_DEBUG=1), and read them in the sketch from here.
#pragma once

/// @defgroup chgame_config Build switches
/// @ingroup lib_chgame
/// @brief CHGAME_DEBUG and CHGAME_PROFILE: set them with build.extra_flags.
///
/// | Switch | Default | What it turns on |
/// |---|---|---|
/// | `CHGAME_DEBUG`   | 0 on the board, 1 in the simulator | the serial debug protocol (@ref chgame_debug): screenshots, input injection, lockstep, perf. Always on in the simulator, which is driven through it; off on the board unless asked for (it costs ~2 KB and needs USB Serial). |
/// | `CHGAME_PROFILE` | 0 | the section profiler (dbg::prof() and the `T` command) |
///
/// @code
/// arduino-cli compile --build-property build.extra_flags=-DCHGAME_DEBUG=1 ...
/// @endcode
/// (`chgame build --debug` does this.) A sketch can test them too, as
/// `#if CHGAME_DEBUG`, to leave things out of its debug build.
/// @{

// CHGAME_DEBUG: the serial debug protocol (chgame/Debug.h).
#ifndef CHGAME_DEBUG
#ifdef CHSIM
#define CHGAME_DEBUG 1
#else
#define CHGAME_DEBUG 0
#endif
#endif

// CHGAME_PROFILE: the section profiler (dbg::prof and the T command).
#ifndef CHGAME_PROFILE
#define CHGAME_PROFILE 0
#endif

/// @}
