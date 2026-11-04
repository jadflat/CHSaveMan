// The registers src/SdSpi.cpp touches, for tests/test_spi.cpp: a byte written
// to SPI1's data register goes to the test's card model and its answer is
// what the register reads back. Build for a 32-bit target: the driver hands
// the DMA addresses as uint32_t (stream(), compiled but never called here).
#pragma once
#include <stdint.h>

uint32_t micros();

struct SpiData {
    uint32_t last = 0xFF;
    SpiData &operator=(uint32_t b);             // clocks one byte (test_spi.cpp)
    operator uint32_t() const { return last; }
};
struct SpiStat {
    operator uint32_t() const { return 1; }     // RXNE at once, never BSY
};
struct SpiRegs {
    uint16_t CTLR1 = 0, CTLR2 = 0;
    SpiStat STATR;
    SpiData DATAR;
};

// BSHR sets pins, BCR clears them; OUTDR is the result.
struct GpioRegs;
struct GpioSet { GpioRegs *p; GpioSet &operator=(uint32_t v); };
struct GpioClr { GpioRegs *p; GpioClr &operator=(uint32_t v); };
struct GpioRegs {
    uint32_t CFGLR = 0x44444444, CFGHR = 0x44444444, OUTDR = 0;
    GpioSet BSHR{this};
    GpioClr BCR{this};
};
inline GpioSet &GpioSet::operator=(uint32_t v) { p->OUTDR |= v; return *this; }
inline GpioClr &GpioClr::operator=(uint32_t v) { p->OUTDR &= ~v; return *this; }

struct DmaChannel { uint32_t CFGR, CNTR, PADDR, MADDR; };
struct DmaRegs { uint32_t INTFR, INTFCR; };

extern SpiRegs t_spi;
extern GpioRegs t_gpioa, t_gpiob;
extern DmaRegs t_dma;
extern DmaChannel t_ch2, t_ch3;
#define SPI1 (&t_spi)
#define GPIOA (&t_gpioa)
#define GPIOB (&t_gpiob)
#define DMA1 (&t_dma)
#define DMA1_Channel2 (&t_ch2)
#define DMA1_Channel3 (&t_ch3)
