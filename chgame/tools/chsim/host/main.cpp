// chsim - runs a CHGame sketch, or any CHGfx sketch, on a PC.
//
// Two ways of running:
//
//   * Lockstep (the default, for sketches on the CHGame library): the sketch
//     starts paused (CHGame::lockstep = 0) and is driven entirely through its
//     serial debug protocol on stdin/stdout, the same protocol the real device
//     speaks over USB. tools/chsim/chdrive.py talks to either. The run is
//     exactly reproducible.
//   * Free-running (`--frames N`, plus --out, --every, --start, --input,
//     --cost, --max-seconds): the sketch runs on virtual time with no driver,
//     buttons come from --input, and every presented frame can be written to
//     a file from the simulated panel (torn frames look torn). For CHGfx's
//     examples, and for seeing what a game's panel shows. A sketch on the
//     CHGame library gets "L0" on stdin first, so it leaves lockstep.
//
// Options of the free run (chsim.py's `run` passes them):
//   --frames N        stop after N presented frames
//   --out FILE        write presented frames to FILE: records of uint32 frame
//                     number, uint32 virtual milliseconds, then 128*128 RGB888
//   --every K         ... every K-th frame (default 1)
//   --start S         ... starting at frame S (default 0)
//   --input SPEC      buttons: "F:BTN+BTN,F:,..." - from presented frame F on,
//                     hold these buttons ("" = none). Buttons: A B UP DOWN LEFT
//                     RIGHT START SELECT. Works in lockstep too (chdrive's K
//                     command is ORed in by the library).
//   --cost            charge the sketch's own CPU time, scaled from the host to
//                     the device with a calibration against the benchmark's
//                     measured primitives (makes a run depend on this PC)
//   --max-seconds T   give up after T seconds of virtual time (default 120)
//
// Serial output goes to stdout; simulator reports and BUG lines to stderr.
// Exit status 3 if any BUG was reported (drawing into rows still being sent,
// the chunk scratch used during a flush), 2 when the time limit stopped it.
#include <Arduino.h>
#include <CHGfx.h>
#include <stdarg.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "sim.h"
#include <chrono>

void setup();
void loop();

SimSerial Serial;
uint32_t sim_ledState = 0;
uint32_t sim_soundEffects = 0, sim_soundScores = 0;

static const uint32_t LOOP_US = 100;    // a pass of loop() on the board, roughly (every recorded frame rests on it)

static uint32_t s_now = 0;
static uint32_t s_idle = 0;
static int s_bugs = 0;
static uint32_t s_frames = 0;           // frames presented
static char s_in[256];
static int s_inLen = 0, s_inPos = 0;

// The free run.
static bool     s_free = false;
static uint32_t s_limit = 300, s_every = 1, s_start = 0;
static uint32_t s_maxUs = 120u * 1000000u;
static FILE    *s_out = nullptr;

// --- Virtual time -----------------------------------------------------------
uint32_t sim_now() { return s_now; }

void sim_advance(uint32_t us) {
    // Whatever the flush converts while time moves on, it converts from the
    // framebuffer as it is now: nothing draws in between.
    s_now += us;
    sim_flushProgress(s_now);
    if (s_free && s_now > s_maxUs) {
        fprintf(stderr, "chsim: %u s of virtual time without reaching %u frames - stopping\n",
                (unsigned)(s_maxUs / 1000000u), (unsigned)s_limit);
        exit(s_bugs ? 3 : 2);
    }
}

void sim_present() { s_idle = 0; }

void sim_bug(const char *fmt, ...) {
    s_bugs++;
    if (s_bugs > 20) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "BUG: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (frame %u, t=%u.%03u ms)\n", (unsigned)s_frames, (unsigned)(s_now / 1000), (unsigned)(s_now % 1000));
    va_end(ap);
}

// --cost: host nanoseconds since the last sync, times the calibrated
// device/host ratio.
static bool     s_cost = false;
static double   s_ratio = 0;
static uint64_t s_hostMark = 0;
static double   s_costCarry = 0, s_costTotalUs = 0;

uint64_t sim_hostNanos() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sim_sync() {
    if (!s_cost) return;
    uint64_t h = sim_hostNanos();
    if (s_hostMark) {
        s_costCarry += (double)(h - s_hostMark) / 1000.0 * s_ratio;
        uint32_t us = (uint32_t)s_costCarry;
        s_costCarry -= us;
        s_costTotalUs += us;
        if (us) sim_advance(us);
    }
    s_hostMark = sim_hostNanos();
}

// Device figures from CHGfx's benchmark, 16 bpp run: gfx_clear, hline x128,
// blit 16x16 transparent, text 24 chars, fillCircle r=30.
static void calibrate() {
    static uint8_t spr[16 * 8];
    for (int i = 0; i < (int)sizeof spr; i++) spr[i] = (uint8_t)(i * 37);
    const double device[5] = { 93, 4, 100, 246, 263 };
    double ratios[5];
    for (int op = 0; op < 5; op++) {
        double best = 1e30;
        for (int rep = 0; rep < 7; rep++) {
            const int N = 400;
            uint64_t t = sim_hostNanos();
            for (int i = 0; i < N; i++) {
                switch (op) {
                    case 0: gfx_clear((uint8_t)(i & 15)); break;
                    case 1: gfx_hline(0, i & 127, 128, (uint8_t)(i & 15)); break;
                    case 2: gfx_blit(spr, i & 63, (i >> 3) & 63, 16, 16, 0); break;
                    case 3: gfx_text(2, 2, "The quick brown fox 0123", 4); break;
                    case 4: gfx_fillCircle(64, 64, 30, (uint8_t)(i & 15)); break;
                }
            }
            double us = (double)(sim_hostNanos() - t) / 1000.0 / N;
            if (us < best) best = us;
        }
        ratios[op] = device[op] / (best > 1e-4 ? best : 1e-4);
    }
    // Median: one op the host happens to be very good or bad at does not skew the rest.
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (ratios[j] < ratios[i]) { double t = ratios[i]; ratios[i] = ratios[j]; ratios[j] = t; }
    s_ratio = ratios[2];
    memset(gfx_fb, 0, GFX_FB_BYTES);
    fprintf(stderr, "chsim: cost model, device/host = %.1f\n", s_ratio);
}

// --- Frames out --------------------------------------------------------------
static void finish() {
    fprintf(stderr, "chsim: %u frames in %u.%03u s virtual (%.1f fps)",
            (unsigned)s_frames, (unsigned)(s_now / 1000000u), (unsigned)(s_now / 1000u % 1000u),
            s_now ? s_frames * 1e6 / s_now : 0.0);
    if (s_cost && s_frames) fprintf(stderr, ", sketch CPU ~%.2f ms/frame", s_costTotalUs / 1000.0 / s_frames);
    fprintf(stderr, ", %d bug%s\n", s_bugs, s_bugs == 1 ? "" : "s");
    if (s_out) fclose(s_out);
    fflush(stdout);
    exit(s_bugs ? 3 : 0);
}

void sim_framePresented() {
    uint32_t n = s_frames++;
    if (!s_free) return;
    if (s_out && n >= s_start && (n - s_start) % s_every == 0) {
        uint32_t hdr[2] = { n, s_now / 1000u };
        fwrite(hdr, sizeof hdr, 1, s_out);
        static uint8_t rgb[GFX_W * GFX_H * 3];
        for (int i = 0; i < GFX_W * GFX_H; i++) {
            rgb[i * 3 + 0] = (uint8_t)(sim_panel[i] >> 16);
            rgb[i * 3 + 1] = (uint8_t)(sim_panel[i] >> 8);
            rgb[i * 3 + 2] = (uint8_t)sim_panel[i];
        }
        fwrite(rgb, sizeof rgb, 1, s_out);
    }
    if (s_frames >= s_limit) finish();
}

// --- Buttons -----------------------------------------------------------------
struct Press { uint32_t frame; uint32_t mask; };
static Press s_input[256];
static int   s_inputs = 0;

static uint32_t buttonBit(uint32_t pin) {
    return (pin >= PIN_BTN_UP && pin <= PIN_BTN_START) ? 1u << (pin - PIN_BTN_UP) : 0;
}

static uint32_t parseButtons(const char *s, const char *end) {
    static const char *names[8] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "SELECT", "START" };
    uint32_t m = 0;
    while (s < end) {
        const char *e = s;
        while (e < end && *e != '+') e++;
        bool ok = false;
        for (int i = 0; i < 8; i++)
            if ((size_t)(e - s) == strlen(names[i]) && !strncmp(s, names[i], (size_t)(e - s))) { m |= 1u << i; ok = true; }
        if (!ok && e > s) { fprintf(stderr, "chsim: unknown button '%.*s'\n", (int)(e - s), s); exit(2); }
        s = e < end ? e + 1 : e;
    }
    return m;
}

static void parseInput(const char *spec) {
    const char *p = spec;
    while (*p && s_inputs < 256) {
        const char *e = strchr(p, ',');
        if (!e) e = p + strlen(p);
        const char *colon = (const char *)memchr(p, ':', (size_t)(e - p));
        if (!colon) { fprintf(stderr, "chsim: bad --input item '%.*s'\n", (int)(e - p), p); exit(2); }
        s_input[s_inputs].frame = (uint32_t)atoi(p);
        s_input[s_inputs].mask = parseButtons(colon + 1, e);
        s_inputs++;
        p = *e ? e + 1 : e;
    }
}

static uint32_t heldMask() {
    uint32_t m = 0;
    for (int i = 0; i < s_inputs; i++) if (s_input[i].frame <= s_frames) m = s_input[i].mask;
    return m;
}

bool sim_buttonHeld(uint32_t pin) { return (heldMask() & buttonBit(pin)) != 0; }

// The CHGame library's button byte (chgame/Input.h: A B UP DOWN LEFT RIGHT
// START SELECT = bits 0..7) from --input; 0 without it, and the driver's K
// command is ORed in by the library.
uint8_t chgame_readButtons() {
    uint32_t m = heldMask();
    if (!m) return 0;
    static const uint8_t to[8] = { 4, 8, 16, 32, 1, 2, 128, 64 };   // UP DOWN LEFT RIGHT A B SELECT START
    uint8_t out = 0;
    for (int i = 0; i < 8; i++) if (m & (1u << i)) out |= to[i];
    return out;
}

// --- Arduino -----------------------------------------------------------------
uint32_t micros() { sim_sync(); return s_now; }
uint32_t millis() { sim_sync(); return s_now / 1000; }
void delay(uint32_t ms) { sim_sync(); sim_advance(ms * 1000); }
void delayMicroseconds(uint32_t us) { sim_sync(); sim_advance(us); }
void pinMode(uint32_t, uint32_t) {}
void digitalWrite(uint32_t pin, uint32_t v) { if (pin == LED_BUILTIN) sim_ledState = v; }
int digitalRead(uint32_t pin) { return sim_buttonHeld(pin) ? LOW : HIGH; }

static uint32_t s_rng = 1;
static uint32_t rng() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
void randomSeed(unsigned long s) { s_rng = s ? (uint32_t)s : 1; }
long random(long hi) { return hi > 0 ? (long)(rng() % (uint32_t)hi) : 0; }
long random(long lo, long hi) { return hi > lo ? lo + random(hi - lo) : lo; }

// --- Serial on stdin/stdout ----------------------------------------------------
static bool refill(bool block) {
    if (s_inPos < s_inLen) return true;
    if (!block || s_free) return false;
    if (!fgets(s_in, sizeof s_in, stdin)) {
        exit(s_bugs ? 3 : 0);       // driver closed the pipe: done
    }
    s_inLen = (int)strlen(s_in);
    s_inPos = 0;
    return s_inLen > 0;
}

// The sketch has nothing to do until the driver speaks (e.g. the CPU's
// search waiting for the next lockstep N).
void sim_waitInput() { refill(true); }

int SimSerial::available() { return refill(false) ? s_inLen - s_inPos : 0; }
int SimSerial::read() { return refill(false) ? (uint8_t)s_in[s_inPos++] : -1; }
size_t SimSerial::write(uint8_t c) { fputc(c, stdout); fflush(stdout); return 1; }
size_t SimSerial::write(const uint8_t *p, size_t n) { fwrite(p, 1, n, stdout); fflush(stdout); return n; }
size_t SimSerial::print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
int SimSerial::printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    print(buf);
    return n;
}

// Debug.cpp's raw output path in simulator builds.
void sim_out(const uint8_t *p, uint32_t n) { fwrite(p, 1, n, stdout); fflush(stdout); }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if      (!strcmp(a, "--frames"))      { s_limit = (uint32_t)atoi(v); s_free = true; i++; }
        else if (!strcmp(a, "--every"))       { s_every = (uint32_t)atoi(v); if (!s_every) s_every = 1; i++; }
        else if (!strcmp(a, "--start"))       { s_start = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--out"))         { s_out = fopen(v, "wb"); if (!s_out) { perror(v); return 2; } i++; }
        else if (!strcmp(a, "--input"))       { parseInput(v); i++; }
        else if (!strcmp(a, "--cost"))        { s_cost = true; }
        else if (!strcmp(a, "--max-seconds")) { s_maxUs = (uint32_t)(atof(v) * 1e6); i++; }
        else { fprintf(stderr, "chsim: unknown option %s\n", a); return 2; }
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (s_free) {
        // A sketch on the CHGame library starts in lockstep; "L0" lets it run.
        strcpy(s_in, "L0\n");
        s_inLen = 3;
    }
    if (s_cost) calibrate();
    sim_sync();
    setup();
    for (;;) {
        // In lockstep the sketch idles between N commands; once it has gone
        // a few loops without presenting a frame, wait for the next command.
        if (!s_free && ++s_idle > 2) refill(true);
        loop();
        sim_sync();
        sim_advance(LOOP_US);
    }
}
uint8_t _ebss;   // stands in for the linker symbol Debug.cpp reports against
