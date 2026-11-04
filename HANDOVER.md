# CHSaveMan: handover for a new session

Written 2026-11-03 (second version). Read this, then `CHSaveMan/README.md` and `CHSaveMan/NOTES.md`.

## What it is

A save game manager app for CHGame (CH32X035, 128x128). Every CHGame game keeps its save in two shared flash
pages (0xF500, 0xF600). The app lists them with the game name, backs them up to `SAVES.BAK` on the SD card,
restores them, erases a page, and scans `GAMES\*.CHG` (and one level of sub-folders) to learn each game's 4-char
save code and name (appended to `SAVENAME.TXT`). GPL-3.0-or-later (the SD driver comes from CHSDtoSerial).
Project root `Arduino\CHSaveMan\`, app in `CHSaveMan\CHSaveMan\` (vendored `chgame/`).

## State: v0.18 (2026-11-04), PC tests only (board and card hold v0.15)

- Release image 50,308 B of 50,432 (124 B free), static RAM 16,892 B. `dist\SAVEMAN.CHG` v0.18: payload crc 0x24660CD5,
  with the menu picture (`docs/cart.png`). `./cl check` ALL GOOD: host_fatfs 159, sim_save 183, extra_safety 59, extra_features 23, sim_round3 126.
- Done in 0.18: every test the PC allows (see NOTES.md "Tests"): the board's card code and FatFs on a RAM disk
  (`tools/tests/host_fatfs.cpp`, `./cl test`) with power cuts after every sector write; NO CARD / CARD NOT FAT32 /
  CARD ERROR on every screen (`SAVM_NOCARD=err`, `Z nocard|nofat|cardfail`); the failed SAVENAME.TXT append; a
  multi-frame scan naming every game; power cuts in SAVES.UND and .SAV writes; read-back failures; a full round trip.
- Fixed: the scan's frame budget (guess = slowest recent call, 12 ms, SCAN_STEP 2 KB, calls stop after a folder or
  a bad file; model 25.5 ms worst, was 32-39 ms); a lone `SAVES/<CODE>.TMP` is listed in RESTORE as its `.SAV`.
- Needs real hardware, still untested: NO CARD with the slot empty, a non-FAT card, a real game's round trip
  (StarLift 0.38 + its STARLIFT.DAT), the scan frame time and duration on the board, a NEW game's SAVENAME.TXT append.
- The menu picture: chgpack `--image` needs `chcart`, which is in the CHGame repo clone's `tools/`, not in the
  vendored `chgame/tools`: pack with the clone's chgpack.py (see Commands), verify with either.

## State: v0.17 (2026-11-04), simulator only (board and card hold v0.15)

- Release image 50,088 B of 50,432 (344 B free), static RAM 16,876 B of 18,416. A debug build no longer fits with both
  save pages. `dist\SAVEMAN.CHG` v0.17: payload 50,088 B, crc 0x5C4276DE, no menu picture (chgpack `--image` needs the
  missing `chcart` module). `sim_save.py` 183 checks, `extra_safety.py` 59, `extra_features.py` 23, `cl check` ALL GOOD.
- Page A UNKNOWN DATA after an Alien Breed save: not explained yet; see `out\improvements3.md` (the 0.17 hex view shows
  the bytes). Alien Breed is built with the older installed core library (no foreign-page protection).
- v0.12 to v0.17 never ran on a board. Feature list with formats: `CHANGELOG.md`, `README.md` ("Files on the
  card"), `out\dev_log.md`, round-2 ideas `out\improvements2.md`. Open: board tests (NEW-game SAVENAME append, NO CARD
  / CARD ERROR, a real-game round trip), the new SCAN_BUDGET 16 ms frame time, a menu picture.
- START hold is 3 s: the app sets `startExits = false` and counts 90 frames itself (the vendored library counts
  180 polls assuming 60 Hz, 6 s at 30 fps; not changed).
- **Names.h is generated and packed** (`"CODE" "NAME" "\0"` entries): never hand-edit, re-run
  `python CHSaveMan/tools/gen_names.py --root <Arduino folder> --root <CHGame repo>/platform/board/arduino/CHGame/libraries/CHGame/examples/Games`
  (the second root is the CHGame examples; hand-made short names live in `tools\names_extra.txt`).

## State: v0.11 (2026-11-04) - FIRST BOARD RUNS (the only version run on a board)

- Image 37,056 B, RAM 15,984 B (release); debug 42,104 B.
- sim_save.py ALL OK (101 checks). New: GAMES LIST + GAMELIST.TXT, scan two sub-folder levels, page chooser for
  BACK UP / RESTORE (BOTH / ONLY A / ONLY B), CMD18 streaming + 24 ms frame budget in the scan, debug `say X` and
  `say D <path>` (hex dump of a card file; lines can get merged on USB, rebuild by offset).
- Verified on the board (COM8, real card, 74 games): scan 14.6 s with right counts, B STOP, rescan, GAMES LIST,
  restore both, back up both with REPLACE YES, erase B, back up only A (backup's B kept), restore only B (A kept),
  every NO path, hold START 6 s leaves the app. SAVES.BAK ended byte-identical to the copy taken first
  (`out\SAVES_card.BAK`: BECK + CHBB). Flash pages after the tests: A BECK #5, B erased.
- Not verified on the board: SAVENAME.TXT append of a NEW game (every card game is already known), NO CARD /
  CARD ERROR, game save round trips (debug builds of CHix, CHSlug, CHFight do not fit; CHStarfin's debug build
  covers page A; StarLift 0.38 needs the new STARLIFT.DAT on the card: "NEED STARLIFT.DAT").
- The card's STARLIFT.CHG is 0.30j (stage-select build, 59,392 B, no flash save): that is why the scan finds no
  code; 0.38 (dist) scans as SLFT. The card's STARLIFT.DAT is older than 0.38's. Not copied (user's call).
- See `out\scan_misses.md` for the 15 games without a code (all expected except the old StarLift build).

## State: v0.9

- Image 33,876 B (limit 50,432 B with both save pages), static RAM 15,640 B of 18,416 B.
- `dist\SAVEMAN.CHG` (payload crc 0x7E195F5E, verified, no menu picture: `chcart` module missing for `--image`),
  `dist\chsaveman_card.img` (64 MB FAT32 test card, label CHSAVEMAN, 11 games + CHFIGHT in GAMES\MORE; not in git; mkcard writes to `out/`, copy it to `dist/`), `dist\MISTER-HANDOFF.md`.
- `python CHSaveMan/tools/tests/sim_save.py`: ALL OK (84 checks) in the PC simulator.
- **Never on a real board** (board needs repair; no flashing/commits/repos until the user says so).
  Compiled but never run: the FatFs scan, SPI1 sharing with the display, the CMD0 probe, `namesAppend`, the
  `._` skip, `promote` of a cut-off .TMP, and the init-error mapping (`chsd_init_err` in `src/sd/disk.cpp`).

## Done in v0.9 (from the Opus review)

BACK UP asks "REPLACE SAVES.BAK?" when a backup or .TMP exists; SAVENAME.TXT is only appended (a file over 1 KB
works; reads give whole lines of the first 1 KB); a lone .TMP is promoted before the next write; a card that
fails init shows CARD ERROR, not NO CARD; empty backup file no longer reads as IO; `._*` files skipped in the
scan; the "LAST LARGE GAME'S CODE" hint removed.

## Still open

- The scan's frame time is modelled (0.18), not measured: check it on the board (`SCAN_BUDGET` in the .ino,
  `SCAN_STEP` in `sdio.h`).
- (Closed in 0.13/0.18: the failed names write leaving names dirty, the multi-frame scan checking every name.)
- MiSTer: the stock sim/RTL (`emu/src/sdspi.c`, `core/rtl/periph/chg_sdspi.sv`) send 0xFF 0xFF as the data CRC16
  but the app's driver checks it, so NO CARD appears. The user said: **no changes to the MiSTer core.** The fix
  exists only in scratchpad copies (`...\scratchpad\mister\`); using those copies for a sim run is fine.
- Same gap in the CHGameRetro emulator; whether to fix it is the user's call.

## MiSTer simulation

Workspace: a scratchpad folder mister (gone with the session)
(CHGameMiSTer untouched; Verilator from oss-cad-suite; `sim/vbuild.sh` builds in ~20 s;
testbench `sim\tb_savm.cpp`; ~90x slower than real time). v0.5 results: stock C model and RTL agree on every frame;
with a CRC16 fix in the scratchpad copies BACK UP wrote a valid 528 B SAVES.BAK and the scan learned names.
v0.9 run (CRC16 fix in scratchpad copies, `...\mister
un8\`) PASSED: 1,272 frames identical C model vs RTL, the two end cards
byte-identical; BACK UP wrote SAVES.BAK, the scan ended "SCAN DONE 13 GAMES 0 NEW, 11 KNOWN", restore ran ("RESTORED FROM
SAVES.BAK"). The v0.5 label oddity "0000000000CHIX" was WCH's printf padding `%.14s` with '0'; v0.9's `cut14` fixes it (page
labels read "BECKON" / "CHIX"). Not seen in any sim: a page really changing after restore (the testbench does not dump flash).

## Commands

    ./cl build                                              flash and RAM (PATH needs w64devkit's bin)
    ./cl run CHSaveMan/tools/scripts/smoke.txt out/smoke    screenshots
    python CHSaveMan/tools/tests/sim_save.py                simulator checks
    python <CHGame repo>/tools/chgpack.py pack CHSaveMan/build/release/CHSaveMan.ino.bin dist/SAVEMAN.CHG --title "SAVE MANAGER" --author Jadflat --version 0.18 --image docs/cart.png
    python chgame/tools/chgpack.py verify dist/SAVEMAN.CHG
    ./cl test                                               host test (board card code + FatFs on a RAM disk)
    ./cl check                                              everything, incl. the four simulator test files
    python CHSaveMan/tools/gen_names.py --root <Arduino folder> --root <CHGame repo>/platform/board/arduino/CHGame/libraries/CHGame/examples/Games

Simulator env: `SAVM_CARD` (folder) and `SAVM_BAK` (file) must be absolute paths.

## Rules (user's)

- No flashing, no commits, no repo creation until the user says so. Don't edit CHGameMiSTer or other projects
  (read only). Bump the version on every build handed over. LF line endings. Be honest about what was not run.
- Use Opus or cheaper workers; Opus review once there is something. Remind the user once to `/compact` when long.
- Python heredocs: `"\n"` inside a heredoc string becomes a real newline (cost this session two build breaks).

## Left to do

- Local git repo (noreply identity), NOT pushed, no GitHub repo. Pre-push scrub done: `scan_truth.txt` paths are relative to the
  folder that holds `Arduino/`; `out/`, `build/` and test cards are git-ignored; no e-mail in tracked files.
- Decided not to touch: the app's CRC16 check (real cards send a real CRC16; the gap is in the MiSTer and CHGameRetro emulators).
- When the board is repaired: a short board test (memory `short-board-tests.md`) on the user's OK only.

## v0.9 (Opus review of v0.8, 2026-11-03)

Review found no data-loss path. Fixed in v0.9: restore question now shows what each page holds now ("A CHIX -> CHAA",
SAME, OLD) and an orange LOSES warning; `._`/tiny files no longer inflate "N GAMES" (counted after the CHG1 header
check; the 8.3 name never had the dot); `sdio::namesFind` searches all of SAVENAME.TXT (names past 1 KB are no longer
re-appended); a failed SAVES.BAK read gives CARD ERROR, not BAD BACKUP (`SAVM_FAIL=read`). Image 33,876 B, RAM 15,640 B,
crc 0x7E195F5E, sim_save.py ALL OK (84 checks). dist\ holds v0.9 (SAVEMAN.CHG and the rebuilt card image).
Run9 (v0.8 + page A erase) passed: C vs RTL identical, 1590 frames; erase dialog "LOSES BECKON SAVE" clean; SAVES.BAK
528 B, tag, crc ok, equal to the seeded pages, same on both cards. Not changed: SCAN_STEP (needs a board), the 8.3 vs long
fallback name. A v0.9 MiSTer sim (run10) was started; v0.9 has not run on a board.

## Run10 result (v0.9 MiSTer sim, 2026-11-04)

Stopped at ~26 of 32.5 sim-seconds (host low on memory); no final summary line. C vs RTL: "pictures match" every second to 26 s.
Scan 13 GAMES 0 NEW 11 KNOWN 0 UNSURE. Restore question with matching pages ("A BECKON SAME", "B CHIX SAME", no LOSES line) and erase dialog
("LOSES BECKON SAVE") fit the dialog, no zeros or clipping. SAVES.BAK 528 B, tag and crc ok, equals pages.bin, both cards identical.
NOT seen: the restore question after the erase (the "->" line and orange LOSES warning), " OLD" rows, "2 GAMES' SAVES".
Rerun with more free RAM (about 2 GB) to cover those.
