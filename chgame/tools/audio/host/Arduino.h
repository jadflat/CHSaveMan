// Host stand-in for the registers the CHGame library's chgame/Audio.cpp
// touches. Every write goes to hw_write(), where harness.cpp models TIM1
// channel 2 on PB10.
#pragma once
#include <stddef.h>
#include <stdint.h>

void hw_write(uint8_t reg, uint32_t value);

struct Reg {
    uint8_t id;
    uint32_t v;
    Reg &operator=(uint32_t x) { v = x; hw_write(id, x); return *this; }
    operator uint32_t() const { return v; }
    Reg &operator|=(uint32_t x) { return *this = v | x; }
    Reg &operator&=(uint32_t x) { return *this = v & x; }
};

enum : uint8_t {
    R_OTHER, R_CTLR1, R_ATRLR, R_CH2CVR, R_SWEVGR, R_CNT, R_CHCTLR1, R_PSC,
};

struct TimRegs {
    Reg CTLR1{R_CTLR1, 0}, CTLR2{R_OTHER, 0}, SMCFGR{R_OTHER, 0}, DMAINTENR{R_OTHER, 0},
        INTFR{R_OTHER, 0}, SWEVGR{R_SWEVGR, 0}, CHCTLR1{R_CHCTLR1, 0}, CHCTLR2{R_OTHER, 0},
        CCER{R_OTHER, 0}, CNT{R_CNT, 0}, PSC{R_PSC, 0}, ATRLR{R_ATRLR, 0}, RPTCR{R_OTHER, 0},
        CH2CVR{R_CH2CVR, 0}, BDTR{R_OTHER, 0};
};
struct GpioRegs { Reg CFGHR{R_OTHER, 0}, BCR{R_OTHER, 0}, BSHR{R_OTHER, 0}; };
struct RccRegs  { Reg APB2PCENR{R_OTHER, 0}; };
struct AfioRegs { Reg PCFR1{R_OTHER, 0}; };

extern TimRegs  *TIM1;
extern GpioRegs *GPIOB;
extern RccRegs  *RCC;
extern AfioRegs *AFIO;

#define RCC_APB2Periph_AFIO  0x01u
#define RCC_APB2Periph_GPIOB 0x08u
#define RCC_APB2Periph_TIM1  0x800u

inline void __disable_irq() {}
inline void __enable_irq() {}
