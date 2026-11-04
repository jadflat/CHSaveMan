// Hello: the smallest complete CHGame sketch.
//
// Steer the ball with the D-pad. A chirps and counts; the count is saved in
// flash and is still there after a power cycle. B re-dyes the felt.
//
// It shows the frame loop every CHGame game uses: logic, then gfx_wait()
// (the previous frame has finished going out to the panel), then drawing
// into the framebuffer, then gfx_flushAsync() (the panel gets it by DMA
// while the next frame's logic runs).
#include <CHGame.h>

// A sound effect is a list of steps: start Hz, end Hz (0: hold), ms.
AUDIO_STEPS(CHIRP) = { AUDIO_STEP(1500, 3000, 40), AUDIO_REST(20), AUDIO_STEP(3000, 0, 40) };
const audio::Effect SOUNDS[] = { AUDIO_EFFECT(CHIRP, 1) };   // priority 1

// What is saved. Every sketch needs a magic of its own: the save pages are
// shared by everything ever uploaded, and each sketch only reads its own.
struct SaveData { uint16_t count; };
const uint32_t MAGIC = save::magic("HELO");
SaveData data;
bool dirty;

int16_t x = 64, y = 72;

void setup() {
    chgame.boot();                       // buttons, and START held 3 s goes back to the menu
    dbg::begin("HELO 1.0");               // the debug protocol's hello (CHGAME_DEBUG builds)
    gfx_begin(GFX_DIV2, GFX_12BPP);       // the panel: 128x128, 16 colours on screen
    pal::init();                          // the house colours (INK, WHITE, FELT, GOLD ...)
    audio::begin(SOUNDS, 1);
    save::load(MAGIC, 1, data);           // leaves data as it was if nothing is saved
    chgame.setFrameRate(60);
}

void loop() {
    dbg::poll();                          // the simulator and tools/device.py talk through this
    if (!chgame.nextFrame()) return;

    // Logic.
    chgame.pollButtons();
    if (chgame.pressed(LEFT_BUTTON) && x > 8) x--;
    if (chgame.pressed(RIGHT_BUTTON) && x < 119) x++;
    if (chgame.pressed(UP_BUTTON) && y > 30) y--;
    if (chgame.pressed(DOWN_BUTTON) && y < 119) y++;
    if (chgame.justPressed(A_BUTTON)) {
        audio::sfx(0);
        data.count++;
        dirty = true;
    }
    if (chgame.justPressed(B_BUTTON)) pal::setTheme((pal::theme() + 1) % pal::THEME_COUNT);
    pal::tick();
    audio::update();

    // Drawing.
    gfx_wait();
    if (dirty) dirty = !save::store(MAGIC, 1, data);    // between gfx_wait() and the flush
    pal::commit();
    gfx_clear(FELT);
    panel(4, 4, 120, 22, 4, FELT_DK, GOLD);
    text35x2s(10, 9, "HELLO", WHITE);
    char buf[8];
    fmtInt(buf, data.count);
    text35(116 - text35Width(buf), 13, buf, GOLD);
    gfx_fillCircle(x + 1, y + 2, 7, FELT_DK);           // a shadow
    gfx_fillCircle(x, y, 7, FX_A);                       // FX_A cycles through a rainbow by itself
    gfx_fillCircle(x - 2, y - 2, 2, WHITE);
    gfx_flushAsync();
}
