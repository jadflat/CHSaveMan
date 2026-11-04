// Runs the CHGame library's real chgame/Audio.cpp, with a game's sounds
// (src/audio/*.cpp: its effects, and its songs through playSong()), on the PC and
// models what the piezo pin does: TIM1 counting at 1 MHz in PWM mode 1 on
// channel 2, with the auto-reload and compare preload registers, update
// events and counter resets. tools/audio/preview.py builds and runs it.
//
//   harness OUT.wav MODE song|sfx INDEX MS [LOG.txt]
//
// MODE is the music rendering (1 arpeggio, 2 lead). A song plays once, to
// its end (MS caps it); an effect plays for MS. Writes a 48 kHz WAV of the
// pin and, optionally, one line per millisecond: "ms hz" (0 = silent).
// Prints the number of times a sounding tone was cut off mid-cycle and
// restarted (each is an audible click), and an FNV hash of the pin's level
// stream.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Arduino.h"
#include <chgame/Audio.h>
extern const audio::Effect SOUNDS[];     // the game's effects (src/audio/Sounds.cpp)
#ifdef PREVIEW_MUSIC
enum class Song : uint8_t;
void playSong(Song s, bool loop);        // the game's songs (a score or a melody)
#endif

static TimRegs tim1; static GpioRegs gpiob; static RccRegs rcc; static AfioRegs afio;
TimRegs *TIM1 = &tim1; GpioRegs *GPIOB = &gpiob; RccRegs *RCC = &rcc; AfioRegs *AFIO = &afio;
extern "C" volatile uint32_t CFGHR_tmpB;
volatile uint32_t CFGHR_tmpB = 0;
extern "C" void osSystickHandler(void);

// ---- TIM1 channel 2 model ----------------------------------------------------
static bool cen, arpe, oc2pe;
static uint32_t arr = 0xFFFF, arrPre = 0xFFFF, ccr, ccrPre, cnt;
static bool level;                       // pin state
static bool ranLastStep;                 // the counter was running a microsecond ago
static uint32_t restarts;                // a running tone reset mid-cycle

void hw_write(uint8_t reg, uint32_t x) {
    switch (reg) {
        case R_CTLR1:
            cen = x & 1; arpe = x & 0x80;
            break;
        case R_CHCTLR1: oc2pe = x & 0x0800; break;
        case R_ATRLR:  arrPre = x & 0xFFFF; if (!arpe) arr = arrPre; break;
        case R_CH2CVR: ccrPre = x & 0xFFFF; if (!oc2pe) ccr = ccrPre; break;
        case R_CNT:    cnt = x & 0xFFFF; break;
        case R_SWEVGR:
            if (x & 1) {                 // UG: reload shadows, restart the count
                // A tone that was mid-cycle and keeps sounding: a clipped cycle.
                if (ranLastStep && cnt != 0 && ccrPre != 0) restarts++;
                cnt = 0; arr = arrPre; ccr = ccrPre;
            }
            break;
        default: break;
    }
}

static void stepMicrosecond() {
    ranLastStep = cen;
    if (cen) {
        level = cnt < ccr;
        if (++cnt > arr) { cnt = 0; arr = arrPre; ccr = ccrPre; }
    } else {
        level = cnt < ccr;               // counter frozen: output holds
    }
}

// ---- WAV --------------------------------------------------------------------
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: harness OUT.wav MODE song|sfx INDEX MS [LOG]\n"); return 2; }
    const char *out = argv[1];
    uint8_t mode = (uint8_t)atoi(argv[2]);
    bool song = !strcmp(argv[3], "song");
    int index = atoi(argv[4]);
    uint32_t ms = (uint32_t)atoi(argv[5]);
    FILE *log = argc > 6 ? fopen(argv[6], "w") : nullptr;

    audio::begin(SOUNDS, 255);
    audio::setMusic(mode == 2 ? audio::LEAD : audio::ARPEGGIO);
    if (song) {
#ifdef PREVIEW_MUSIC
        playSong((Song)index, false);
#else
        fprintf(stderr, "this game has no music\n");
        return 2;
#endif
    } else {
        audio::sfx((uint8_t)index);
    }

    std::vector<int16_t> pcm;
    pcm.reserve(ms * 48 + 16);
    double acc = 0, n = 0, next = 1e6 / 48000.0, t = 0;
    double hpX = 0, hpY = 0;
    uint64_t hash = 1469598103934665603ull;
    uint32_t tail = 0;
    for (uint32_t m = 0; m < ms; m++) {
        if (song && !audio::musicPlaying() && ++tail > 200) break;   // the end, and a little after
        osSystickHandler();              // the 1 kHz SysTick hook
        if (log) fprintf(log, "%u %u\n", m, cen && ccr ? 1000000u / (arr + 1) : 0u);
        for (int us = 0; us < 1000; us++) {
            stepMicrosecond();
            hash = (hash ^ (uint64_t)level) * 1099511628211ull;
            acc += level; n++; t++;
            if (t >= next) {
                double x = acc / n - 0.5;            // -0.5 .. 0.5
                double y = x - hpX + 0.995 * hpY;    // DC block
                hpX = x; hpY = y;
                double s = y * 0.8 * 32767.0;
                if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                pcm.push_back((int16_t)s);
                acc = n = 0; next += 1e6 / 48000.0;
            }
        }
    }
    if (log) fclose(log);

    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    uint32_t bytes = (uint32_t)pcm.size() * 2;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 48000); put32(f, 96000); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, bytes);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    printf("restarts=%u hash=%016llx\n", restarts, (unsigned long long)hash);
    return 0;
}
