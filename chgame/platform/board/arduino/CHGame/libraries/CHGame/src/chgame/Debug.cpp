// The serial debug protocol: see Debug.h.
#include "Debug.h"
#if CHGAME_DEBUG
#include <Arduino.h>
#include <CHGfx.h>
#include <string.h>
#include "Input.h"
#include "Fmt.h"

#ifndef CHSIM
extern "C" {
uint8_t CDC_write_nb(char c);
void CDC_flush(void);
uint8_t CDC_enumerated(void);
uint8_t CDC_dtr(void);
void chgame_enter_bootloader(void);
}
#else
void sim_out(const uint8_t *p, uint32_t n);
void sim_waitInput();
uint64_t sim_hostNanos();
// Render cost in the simulator: host nanoseconds (the simulator's clock
// stands still while a frame is drawn; tools/chsim/perf.py turns them into
// device milliseconds with a calibration against CHGfx's benchmark).
static uint64_t pcT0, pcSum, pcMax;
#endif

namespace dbg {

// Stack high-water mark: the stack is painted at boot, and P reports how
// deep anything has reached since.
static uint32_t *fstkLo, *fstkHi;
#ifndef CHSIM
extern "C" uint32_t _susrstack[], _eusrstack[];
static const uint32_t PAINT = 0xA5A5A5A5u;
static inline __attribute__((always_inline)) void paint(uint32_t *p, uint32_t *end) {
    while (p < end) *p++ = PAINT;
}
static uint32_t stackUsed(uint32_t *lo, uint32_t *hi) {
    uint32_t *p = lo;
    while (p < hi && *p == PAINT) p++;
    return (uint32_t)((uint8_t *)hi - (uint8_t *)p);
}
void frameStack(uint32_t *lo, uint32_t *hi) { fstkLo = lo; fstkHi = hi; paint(lo, hi); }
#else
static uint32_t stackUsed(uint32_t *, uint32_t *) { return 0; }
static uint32_t *_susrstack, *_eusrstack;
void frameStack(uint32_t *lo, uint32_t *hi) { fstkLo = lo; fstkHi = hi; }
#endif

bool (*hook)(char cmd, const char *args) = nullptr;
static const char *helloLine = "CHGAME";
static bool (*holdBusy)(char cmd) = nullptr;

static char line[LINE];
static char *held;                      // a game command waiting for holdBusy to let go
static uint8_t len = 0;
static bool ackPending = false;
static uint32_t tRnd;
static uint32_t sumRnd, maxRnd, frames, late;
static uint32_t lastFrameStart;

// The crash before the last restart, as the core's fault handler kept it at
// the bottom of the stack (chgame_fault() in the core's chgame_boot.c): magic
// "CHGF", mcause, mepc, mtval, ra, sp. Taken before the stack is painted over.
static uint32_t fault[6];
static const uint32_t FAULT_MAGIC = 0x46474843u;

void begin(const char *hello) {
    helloLine = hello;
#ifndef CHSIM
    if (_susrstack[0] == FAULT_MAGIC)
        for (int i = 0; i < 6; i++) fault[i] = _susrstack[i];
    // Up to the stack pointer itself (less 16 B), never "below a local of
    // this function": with LTO this is inlined into main(), whose frame
    // reaches further down than any local here, and painting over it put
    // 0xA5A5A5A5 into main()'s saved values (CHSnakes' debug build faulted
    // on its first frame, 2026-10-02).
    uint32_t *sp;
    __asm volatile ("mv %0, sp" : "=r"(sp));
    paint(_susrstack, sp - 4);
#endif
}

void holdInto(bool (*busy)(char cmd), char *buf) { holdBusy = busy; held = buf; }

#if CHGAME_PROFILE
static uint32_t profT, profSum[12], profFrames;
// The simulator's clock stands still while it draws: there, host
// nanoseconds, for the sections' shares rather than their times.
#ifdef CHSIM
static uint32_t profNow() { return (uint32_t)sim_hostNanos(); }
#else
static uint32_t profNow() { return micros(); }
#endif
void profStart() { profT = profNow(); profFrames++; }
void prof(uint8_t slot) {
    uint32_t now = profNow();
    if (slot < 12) profSum[slot] += now - profT;
    profT = now;
}
#endif

#ifndef CHSIM
// Bulk writes go straight to the CDC endpoint in 64-byte packets: the core's
// Serial.write() flushes after every byte.
static void out(const uint8_t *p, uint32_t n) {
    if (!CDC_enumerated() || !CDC_dtr()) return;
    while (n) {
        uint32_t t = millis();
        while (!CDC_write_nb((char)*p)) {
            if (!CDC_enumerated() || millis() - t > 25) return;
        }
        p++; n--;
    }
    CDC_flush();
}
#else
static void out(const uint8_t *p, uint32_t n) { ::sim_out(p, n); }
#endif

void print(const char *s) { out((const uint8_t *)s, (uint32_t)strlen(s)); }

uint32_t parseNum(const char *&p, uint8_t base) {
    uint32_t v = 0;
    while (*p == ' ' || *p == ',') p++;
    for (;; p++) {
        char c = *p;
        uint8_t d;
        if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
        else break;
        v = v * base + d;
    }
    return v;
}

// "KEY=value" pairs, one line.
static char *kv(char *p, const char *key, uint32_t v) {
    p = fmtStr(p, key);
    return fmtInt(p, (int32_t)v);
}

// The same, the value in hex (addresses).
static char *kx(char *p, const char *key, uint32_t v) {
    p = fmtStr(p, key);
    for (int s = 28; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(v >> s) & 15];
    *p = 0;
    return p;
}

static void runHook(const char *l) {
    const char *args = l + 1;
    while (*args == ' ') args++;
    print(hook && hook(l[0], args) ? "OK\n" : "ERR\n");
}

#ifdef CHSIM
// Q (the simulator): host nanoseconds for the primitives the CHGfx
// benchmark measured on the board (its benchmark-results.txt), so chdrive's
// `cal` can turn the simulator's render times into estimated device times.
// The frame being shown is put back afterwards.
static void calibrate(char *buf) {
    static uint8_t keep[GFX_FB_BYTES], spr[8 * 16];
    gfx_wait();
    memcpy(keep, gfx_fb, sizeof keep);
    memset(spr, 0x3F, sizeof spr);
    uint64_t t0, r[5];
    t0 = sim_hostNanos(); for (int i = 0; i < 200; i++) gfx_clear((uint8_t)i); r[0] = (sim_hostNanos() - t0) / 200;
    t0 = sim_hostNanos(); for (int i = 0; i < 20000; i++) gfx_hline(0, i & 127, 128, (uint8_t)i); r[1] = (sim_hostNanos() - t0) / 20000;
    t0 = sim_hostNanos(); for (int i = 0; i < 2000; i++) gfx_blit(spr, i & 63, i & 63, 16, 16, 15); r[2] = (sim_hostNanos() - t0) / 2000;
    t0 = sim_hostNanos(); for (int i = 0; i < 1000; i++) gfx_text(0, i & 63, "ABCDEFGHIJKLMNOPQRSTUVWX", 1); r[3] = (sim_hostNanos() - t0) / 1000;
    t0 = sim_hostNanos(); for (int i = 0; i < 1000; i++) gfx_fillCircle(64, 64, 30, (uint8_t)i); r[4] = (sim_hostNanos() - t0) / 1000;
    memcpy(gfx_fb, keep, sizeof keep);
    char *p = fmtStr(buf, "CAL");
    for (int k = 0; k < 5; k++) { *p++ = ' '; p = fmtInt(p, (int32_t)r[k]); }
    fmtStr(p, "\nOK\n");
    print(buf);
}
#endif

static void execute() {
    line[len] = 0;
    char cmd = line[0];
    const char *args = line + 1;
    while (*args == ' ') args++;
    char buf[112];
    char *p = buf;
    switch (cmd) {
        case '?':
            fmtStr(fmtStr(p, helloLine), "\n");
            print(buf);
            break;
        case 'S': {
            gfx_wait();
            p = kv(p, "FB ", chgame.frameCount);
            fmtStr(p, " 8224\n");
            print(buf);
            out(gfx_fb, GFX_FB_BYTES);
            uint16_t pal[16];                       // as the panel shows it, fade included
            for (uint8_t i = 0; i < 16; i++) pal[i] = gfx_paletteOut(i);
            out((const uint8_t *)pal, sizeof pal);  // one write: out() ends in a USB flush
            break;
        }
        case '!':
            p = fmtStr(p, "FAULT");
            if (fault[0] == FAULT_MAGIC) {
                p = kx(p, " mcause=", fault[1]);
                p = kx(p, " mepc=", fault[2]);
                p = kx(p, " mtval=", fault[3]);
                p = kx(p, " ra=", fault[4]);
                p = kx(p, " sp=", fault[5]);
            } else {
                p = fmtStr(p, " none");
            }
            fmtStr(p, "\n");
            print(buf);
            break;
        case 'K':
            chgame.injected = (uint8_t)parseNum(args, 16);
            print("OK\n");
            break;
        case 'L':
            chgame.lockstep = (*args == '1') ? 0 : -1;
            print("OK\n");
            break;
        case 'N':
            if (chgame.lockstep < 0) chgame.lockstep = 0;
            chgame.lockstep += (int32_t)parseNum(args, 10);
            ackPending = true;
            break;
        case 'P': {
            uint32_t f = frames ? frames : 1;
            p = kv(p, "PERF rnd=", sumRnd / f);
            p = kv(p, " max=", maxRnd);
            p = kv(p, " late=", late);
            p = kv(p, " frames=", frames);
            p = kv(p, " stk=", stackUsed(_susrstack, _eusrstack));
            if (fstkLo) p = kv(p, " fstk=", stackUsed(fstkLo, fstkHi));
#ifdef CHSIM
            p = kv(p, " pcrnd=", (uint32_t)(pcSum / f));
            p = kv(p, " pcmax=", (uint32_t)pcMax);
            pcSum = pcMax = 0;
#endif
            fmtStr(p, "\n");
            print(buf);
            sumRnd = maxRnd = frames = late = 0;
            break;
        }
#if CHGAME_PROFILE
        case 'T': {
            uint32_t f = profFrames ? profFrames : 1;
            p = fmtStr(p, "PROF");
            for (int i = 0; i < 12; i++) {
                if (!profSum[i]) continue;
                *p++ = ' ';
                p = fmtInt(p, i);
                p = kv(p, "=", profSum[i] / f);
                profSum[i] = 0;
            }
            fmtStr(p, "\n");
            print(buf);
            profFrames = 0;
            break;
        }
#endif
#ifdef CHSIM
        case 'Q':
            calibrate(buf);
            break;
#endif
#ifndef CHSIM
        case 'B':
            chgame_enter_bootloader();
            break;
#endif
        default:
            // The game is busy: answer HELD now, OK/ERR once it has run.
            if (holdBusy && holdBusy(cmd)) { memcpy(held, line, LINE); print("HELD\n"); break; }
            runHook(line);
            break;
    }
}

void waitInput() {
#ifdef CHSIM
    sim_waitInput();
#endif
}

void poll() {
    // A held game command runs once the game lets go, before the frame ack.
    if (held && held[0] && !holdBusy(held[0])) {
        runHook(held);
        held[0] = 0;
    }
    if (ackPending && chgame.lockstep == 0) {
        ackPending = false;
        char buf[20];
        fmtStr(fmtInt(fmtStr(buf, "OK "), (int32_t)chgame.frameCount), "\n");
        print(buf);
    }
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (len) execute();
            len = 0;
        } else if (c >= 32 && c < 127 && len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;    // binary noise: drop the line
        }
    }
}

#ifndef CHSIM
// After a crash the core's fault handler comes here, interrupts on and the
// stack fresh: the protocol goes on answering ('!' this crash, 'S' the last
// frame drawn, 'B' to the bootloader) until the board is reset.
extern "C" __attribute__((noreturn)) void chgame_fault_park() {
    for (int i = 0; i < 6; i++) fault[i] = _susrstack[i];
    hook = nullptr;                     // the game's own commands could crash again
    holdBusy = nullptr;
    held = nullptr;
    for (;;) poll();
}
#endif

void markUpdateStart() {
    uint32_t now = micros();
    if (frames && chgame.lockstep < 0 && now - lastFrameStart > 17500) late++;
    lastFrameStart = now;
}
void markRenderStart() {
    tRnd = micros();
#ifdef CHSIM
    pcT0 = sim_hostNanos();
#endif
}
void markRenderEnd() {
#ifdef CHSIM
    uint64_t d = sim_hostNanos() - pcT0;
    pcSum += d;
    if (d > pcMax) pcMax = d;
#endif
    uint32_t r = micros() - tRnd;
    sumRnd += r;
    if (r > maxRnd) maxRnd = r;
    frames++;
}

}  // namespace dbg
#endif
