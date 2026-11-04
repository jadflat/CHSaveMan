# CHGame library

**API reference: <https://bateske.github.io/CHGame/group__lib__chgame.html>** (every call, its parameters and what it does).

The one include for a CHGame sketch:

```cpp
#include <CHGame.h>
```

It sits on CHGfx (the framebuffer, the panel and its DMA) and gives a game
everything the twenty casino games in `examples/Games/` share: buttons and frame
pacing in the Arduboy style, the house palette, drawing helpers and the 3x5
font, outlined lettering, effects maths, one sound engine, saving to flash,
and the serial debug protocol that the simulator and the tools drive a game
through. The games are its examples: `examples/Games/` holds the twenty of
them, `examples/Apps/` the sketches that are not games (CHStlView, a 3D model
viewer, and CHSDtoUSB, which is GPL-3.0 and carries its own licence), and `examples/Hello` the smallest
complete sketch.

Coming from the Arduboy? Read [docs/getting-started.md](../../../../../../docs/getting-started.md)
first: it maps Arduboy2 calls to these.

## A whole sketch

[examples/Hello/Hello.ino](examples/Hello/Hello.ino) is the smallest complete
sketch: a ball to steer, a sound effect, a counter saved in flash, the felt
re-dyed with B. It shows the frame loop every CHGame game uses:

```cpp
void loop() {
    dbg::poll();                       // the debug protocol (nothing in a release build)
    if (!chgame.nextFrame()) return;
    chgame.pollButtons();
    // ... logic ...
    pal::tick();
    audio::update();

    gfx_wait();                        // the previous frame has gone out: draw
    pal::commit();
    // ... draw into the framebuffer ...
    gfx_flushAsync();                  // DMA to the panel while the next frame's logic runs
}
```

There is one framebuffer. Draw only between `gfx_wait()` and
`gfx_flushAsync()`: before `gfx_wait()` the previous frame may still be
going out, and the simulator reports a `BUG:` line if a frame draws into rows
still being sent. The bigger games run up to three logic ticks per drawn
frame when drawing falls behind, so the game keeps its speed; see
CHCraps' `.ino`.

## Building and running

From the repository root (or a game's folder, where `chgame` does
the same for that game):

| What | Command |
|---|---|
| Release build + size | `chgame --sketch <dir> build` |
| Debug build (protocol on) | `chgame --sketch <dir> build --debug` |
| Upload | `chgame --sketch <dir> upload [--debug]` |
| Run in the simulator | `chgame --sketch <dir> run <script.txt> <outdir>` |
| Sound effects to WAV | `python tools/audio/preview.py <dir> <outdir>` |

The simulator compiles the sketch, CHGfx and this library for the PC and
runs it in lockstep, so a script (`wait`, `tap`, `hold`, `snap`, `gif` ...;
the list is at the top of `tools/chsim/chdrivelib.py`) gives the same
screenshots every time. Builds use the copies of CHGfx and this library in
`platform/board/arduino/CHGame/libraries`. The board package (0.3.0 on)
carries the same copies, so a sketch in the Arduino IDE or a plain
`arduino-cli compile` needs nothing else; with 0.2.4, pass `--library` for both.

## The modules

`CHGame.h` includes all of them; a pure-logic file (host tests, tools) can
include just the one it needs, e.g. `<chgame/Fmt.h>`.

| Header | What |
|---|---|
| `chgame/Input.h` | `chgame`: `boot()`, `setFrameRate()`, `nextFrame()`, `pollButtons()`, `pressed()`, `justPressed()`, `justReleased()`, `repeat()` (auto-repeat), `everyXFrames()`, `frameCount`. Holding START for 3 s goes back to the SD game menu (`startExits = false` opts out). |
| `chgame/Palette.h` | The sixteen house colours by name (`INK`, `WHITE`, `FELT_DK`, `FELT`, `FELT_LT`, `SILVER`, `RED`, `WINE`, `GOLD`, `WOOD`, `BLUE`, `NAVY`, `SKIN`, `CYAN`, `FX_A`, `FX_B`) and `pal::`: felt themes, fades, flashes, desaturation, and FX_A/FX_B colour cycling. A colour change recolours every pixel of that index for free. |
| `chgame/Draw.h` | `fillRound`, `roundRect`, `panel`, `panelLit`, `bevel`, `dither`, `spotlight` (a dotted disc), `dropShadow`, `remapRect`, `fillConvex`, span sprites (`sprite4`, scaled, flipped, recoloured; `spriteRot`), and the 3x5 font: `text35`, `text35s` (shadowed), `text35x2`, `text35Width`. |
| `chgame/Mask.h` | Lettering and logos with an outline, a drop shadow and a gradient fill: `maskBegin`, `maskText35`, `maskBlit1`, `maskDraw`. A title in its cover's lettering (extrusion, ink outline and shadow, the house gold as a dithered gradient baked into its data): `maskTitle`, `titleArt`, with the arrays `tools/titleart.py` writes from the cover recipe. |
| `chgame/Fx.h` | `fx::ease` (cubic, back, in-out, bounce), `isin`/`icos` (integer, 256 steps a turn), `rnd`/`rndRange`/`reseed` (presentation randomness, repeatable), and the screen shake. |
| `chgame/Sizzle.h`, `Sizzle.inl` | The particle pool (`spawn`, `burst`, `fountain`; sparks, confetti, stars, dust, coins, rain, goo), the pop-up or drop-in banner (`banner`, `holdBanner`, `activeRows`) and the floating `+$15` texts (`floatText`). An implementation header: a game sets `SIZZLE_*` switches in its `src/fx/Fx.h` (pool size, kinds, banner style, floats or not) and includes the bodies once from its `src/fx/Fx.cpp`, so each game compiles only what it uses, under its own size pragma. Not part of `<CHGame.h>`; `Sizzle.h` lists the switches. |
| `chgame/Audio.h` | `audio::`: effects as step tables (`AUDIO_STEPS`, `AUDIO_STEP(hz, endHz, ms)`, `AUDIO_EFFECT(steps, priority \| SOFT \| GLIDE)`), `sfx()` (optionally a few semitones up), `blip()`, `note()`, music from Playtune scores (`music()`, its voices rendered as an arpeggio or a lead line) or one-voice melodies (`melody()`), and the status LED. One effect at a time on the piezo; a higher priority wins. |
| `chgame/Save.h` | `save::load(MAGIC, version, data)` / `save::store(...)`: a record of up to 244 bytes in flash, kept across power cycles and re-uploads, two pages in turn with a CRC. Every sketch needs its own magic (`save::magic("XXXX")`). |
| `chgame/Fmt.h` | Numbers to text without printf: `fmtInt`, `fmtMoney`, `fmtCash`, `fmtShort`, `fmtTime`, `fmtStr`. |
| `chgame/RamFunc.h` | `RAMFUNC(name)`: a function that runs from SRAM, about three times as fast as from flash (and costs its size in RAM). For per-pixel loops. |
| `chgame/Debug.h` | `dbg::`: the serial debug protocol (`?` hello, `S` screenshot, `K` buttons, `L`/`N` lockstep, `P` perf, `B` bootloader, and the game's own commands through `dbg::hook`). Built in only when `CHGAME_DEBUG` (always in the simulator; `device.py build --debug` on the board). |
| `chgame/Config.h` | The library's switches: `CHGAME_DEBUG`, `CHGAME_PROFILE`. Set them with `build.extra_flags`, never in a sketch header: the library is compiled apart and cannot see it. |

## Rules of thumb

- **Flash is the budget.** The image may use 50,944 B; keep it at or under
  50,432 B to keep both save pages. `device.py build` prints both after every
  build, and `python tools/check_size.py <dir>/build/release --top 30` says
  what fills it. Unused parts of the library cost nothing (link-time optimisation
  drops them).
- **RAM is 18,416 B** for statics, plus a 2 KB stack. The framebuffer takes
  8 KB of it when the sketch draws with CHGfx. A sketch that includes
  `CHGame.h` for its sound, buttons or saves and draws through its own code
  does not carry it: the libraries link from archives, so an object is
  linked only when something calls into it.
- **`static const` tables of 8 bytes or less** are copied into SRAM by this
  core's link script; sound steps use `AUDIO_STEPS` to stay in flash.
- **Integer maths only.** Soft-float costs kilobytes and frame rate.
- **The debug protocol costs about 2 KB** and needs USB Serial; a release
  build has none of it, and `dbg::` calls compile to nothing.
- **Presentation randomness** (`fx::rnd`) is separate from a game's own
  random numbers, so scripts and tests repeat.

## What stays in a game

The rules, the screens and everything they animate (`stage/`, `render/`,
`fx/Presenter`), its sounds and music tables, its art, its save data and
magic, its debug commands, its `config.h`, and the `src/fx/Fx.h` that
configures `chgame/Sizzle` for it (plus any effect of its own on top, such
as CHBingo's `gack()`). The twenty games in `examples/Games/` show the
range, from CHTicTacToe to CHChess's search running inside the frame loop;
each `NOTES.md` explains its design.

## License

Apache-2.0 (`LICENSE`). The 3x5 pixel font is Press Play On Tape's; see
`NOTICE`.
