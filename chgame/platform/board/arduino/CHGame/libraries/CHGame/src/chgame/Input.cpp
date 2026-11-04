#include <Arduino.h>
#include "Input.h"

CHGame chgame;

#ifndef CHSIM
// Direct INDR reads: one register load per port instead of eight
// digitalRead() calls through the pin map.
uint8_t chgame_readButtons() {
    uint32_t b = ~GPIOB->INDR, c = ~GPIOC->INDR;
    uint8_t m = 0;
    if (b & (1u << 1))  m |= A_BUTTON;
    if (b & (1u << 6))  m |= B_BUTTON;
    if (b & (1u << 4))  m |= UP_BUTTON;
    if (c & (1u << 14)) m |= DOWN_BUTTON;
    if (b & (1u << 3))  m |= LEFT_BUTTON;
    if (c & (1u << 15)) m |= RIGHT_BUTTON;
    if (b & (1u << 8))  m |= START_BUTTON;
    if (b & (1u << 7))  m |= SELECT_BUTTON;
    return m;
}
#endif

#ifndef CHSIM
extern "C" volatile uint32_t CFGHR_tmpB, CFGHR_tmpC;   // CFGHR is write-only: use the core's shadows

// Input with pull-up is CNF=10, MODE=00 (nibble 8) with the ODR bit set.
// Registers instead of pinMode(): pinMode drags in ~2 KB of pin-map tables.
// NIB/PU: a pin's nibble in its CFGLR/CFGHR, and "input with pull" in it.
#define NIB(n) (15u << ((n) * 4))
#define PU(n)  (8u << ((n) * 4))
#endif

void CHGame::boot() {
#ifndef CHSIM
    // Switches to GND, no external pull-ups. One masked write per register:
    // PB1 A, PB3 LEFT, PB4 UP, PB6 B, PB7 SELECT (CFGLR), PB8 START (CFGHR),
    // PC14 DOWN, PC15 RIGHT (CFGHR).
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC;
    GPIOB->CFGLR = (GPIOB->CFGLR & ~(NIB(1) | NIB(3) | NIB(4) | NIB(6) | NIB(7))) |
                   PU(1) | PU(3) | PU(4) | PU(6) | PU(7);
    CFGHR_tmpB = (CFGHR_tmpB & ~NIB(0)) | PU(0);
    GPIOB->CFGHR = CFGHR_tmpB;
    GPIOB->BSHR = (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 8);
    CFGHR_tmpC = (CFGHR_tmpC & ~(NIB(6) | NIB(7))) | PU(6) | PU(7);
    GPIOC->CFGHR = CFGHR_tmpC;
    GPIOC->BSHR = (1u << 14) | (1u << 15);
#endif
    cur = prev = chgame_readButtons();
}

void CHGame::setFrameRate(uint8_t fps) {
    period = 1000000u / fps;
    next = micros() + period;
}

bool CHGame::nextFrame() {
    if (lockstep >= 0) {
        if (lockstep == 0) return false;
        lockstep--;
        frameCount++;
        return true;
    }
    uint32_t now = micros();
    if ((int32_t)(now - next) < 0) return false;
    next += period;
    // Fell far behind (debug pause, a save): resync instead of sprinting.
    if ((int32_t)(now - next) > (int32_t)(3 * period)) next = now + period;
    frameCount++;
    return true;
}

void CHGame::pollButtons() {
    prev = cur;
    cur = (uint8_t)(chgame_readButtons() | injected);
    for (uint8_t i = 0; i < 8; i++) {
        if (cur & (1u << i)) { if (held[i] < 0xFFFF) held[i]++; }
        else held[i] = 0;
    }
    if (startExits && held[6] >= 180) chgame_exitToMenu();     // START, 3 s of 60 Hz ticks
}

void chgame_exitToMenu() {
#ifdef CHSIM
    fprintf(stderr, "chsim: START held 3 s: exit to the menu\n");
    exit(0);
#else
    NVIC_SystemReset();             // no request: the menu bootloader shows the menu
    for (;;) { }
#endif
}

bool CHGame::repeat(uint8_t b, uint8_t delay, uint8_t rate) const {
    for (uint8_t i = 0; i < 8; i++) {
        if (!(b & (1u << i))) continue;
        uint16_t h = held[i];
        if (h == 1) return true;
        if (h > delay && ((h - delay) % rate) == 0) return true;
    }
    return false;
}
