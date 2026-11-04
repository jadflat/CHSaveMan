// chsim internals shared by the host shims (main.cpp, chgfx_host.cpp, CHSd's
// sd_host.cpp, and CHGfx's extras/tests, which provide their own hooks).
#pragma once
#include <stdint.h>

uint32_t sim_now();                     // virtual microseconds
void     sim_advance(uint32_t us);      // move virtual time on (the flush in flight lands the rows it converts)
void     sim_sync();                    // --cost: charge the sketch's host time since the last sync, scaled to the device
void     sim_present();                 // a flush started: the sketch is drawing (lockstep's idle counter)
void     sim_framePresented();          // a flush has fully landed on the panel
void     sim_flushProgress(uint32_t now);   // chgfx_host.cpp: land the rows converted by `now`
void     sim_bug(const char *fmt, ...); // report a correctness bug (exit code 3)
void     sim_waitInput();               // lockstep: block until the driver sends more input
uint64_t sim_hostNanos();               // real PC time, for the render cost estimate
bool     sim_buttonHeld(uint32_t pin);  // --input: is this button down (PIN_BTN_*)
uint32_t sim_cardBlocks();              // the pretend SD card's file in 512 B blocks (0: no card)
void     sim_cardEject(bool out);       // pull the card out / put it back

// The panel: what the ST7735 is showing, RGB888, as the rows landed.
extern uint32_t sim_panel[];
