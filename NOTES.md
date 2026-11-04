# CHSaveMan notes

## How to check a new game

1. Pack the game as usual and put the `.CHG` in `GAMES\` (or one sub-folder) on the SD card.
2. Run CHSaveMan, pick FIND GAME NAMES. The scan reads each `.CHG` and looks for the 4-char save code
   it passes to `save::read/write`; the title comes from the `.CHG` header.
3. The result line shows `N GAMES  n NEW  n KNOWN  n UNSURE`.
   - NEW: the code and title were appended to `SAVENAME.TXT` as `CODE=TITLE`.
   - KNOWN: the code is already in the built-in table or `SAVENAME.TXT`.
   - UNSURE: the file has two or more equal-scoring codes; nothing is learned. Add the line to
     `SAVENAME.TXT` by hand.
4. A game with no save code is counted but learns nothing. A non-ASCII title uses the file name.
5. `SAVENAME.TXT` is only ever appended to; lines the app does not use are kept. Only the first 32 names
   are used (NAME LIST FULL appears when more are found).

## Tests

`./cl check` runs everything (ALL GOOD at 0.18): the host test, the scripts, the four simulator test files
(`tools/game.py` lists them) and the device build. On their own:

- `./cl test`: `tools/tests/host_fatfs.cpp`, 159 checks. The board's card code (`src/sd/sdio_dev.cpp`) and FatFs
  with the board's `ffconf.h`, on a RAM disk formatted FAT16 (2 KB and 32 KB clusters) and FAT32 (MBR, 512 B
  clusters): NO_CARD / IO / NOT_FAT mounts; SAVES.BAK / .BK1 / .UND, the SAVES folder, .SAV / .SV1 renames;
  the disk image after every sector write of a backup, an undo, a .SAV and a SAVENAME.TXT append (each reads as
  the old or the new file, the lone .TMP case included, and the next write recovers); a failing sector write at
  every point (IO, nothing lost); the GAMES scan (every code, codes on chunk edges, deep / junk / bad files, a
  remount and a pulled card mid-scan); a frame-time model of the app's scan loop.
- `python CHSaveMan/tools/tests/sim_save.py` (183), `extra_safety.py` (59), `extra_features.py` (23),
  `sim_round3.py` (126: every screen with NO CARD / CARD NOT FAT32 / CARD ERROR at boot and at run time, a
  failed SAVENAME.TXT append written once later, a multi-frame scan naming every game, power cuts in the undo
  and .SAV writes, read-back failures, a full round trip byte for byte).
- Simulator faults (`src/sd/sdio_sim.cpp`): `SAVM_NOCARD=1|fat|err`, `SAVM_FAIL=io|crc|scan|append|read|cut`,
  at run time `say Z <mode>` (also `nocard`, `nofat`, `cardfail`; `Z -` back to `$SAVM_FAIL`).

The app has run on a real board only at v0.11 (see HANDOVER).

## Scan frame time (0.18)

The frame is the panel flush (11.1 ms; SPI1 is shared, so card reads cannot overlap it), the drawing and the card
time. 0.11 to 0.17 guessed the next scanNext call's time from the last call, so a slow call could start near the
end of the budget (board 0.11: 40 ms frames with a 24 ms budget; one 4 KB call took up to 20.5 ms). 0.18 guesses
the slowest recent call (decaying 1/16 a call), SCAN_BUDGET 12 ms, SCAN_STEP 2 KB, and a call stops after a folder
opens, a bad .CHG header or 16 folder entries (at most 6 sectors a call in the host test). The host model, from the
sectors each call reads and the board's ~0.8 ms a sector: worst frame 25.5 ms (4 KB step: 25.5; 0.17's guess with
a 16 ms budget: 32-39 ms), 25.1 ms if the card is twice as slow, 30.7 ms if one call is suddenly twice as slow. A
model, not a measurement: confirm on the board (a debug build does not fit with both save pages; build one with
one save page to read `STATE scan ... framemaxus`).

## v0.10 (2026-11-04)

- The scan reads GAMES, its sub-folders and theirs (GAMES\OTHER\CARDS); each scan rewrites `GAMELIST.TXT`
  (`FILE.CHG CODE NAME`, `-` for no code). GAMES LIST pages that file from the card (UP/DOWN, LEFT/RIGHT, B).
- BACK UP / RESTORE ask BOTH PAGES, ONLY A or ONLY B (same letter only). A one-page backup keeps the other
  page of SAVES.BAK (blank if the old file is bad); a one-page restore leaves the other flash page alone.
- Board (real card, 74 games): scan 14.6 s. The card read stays streaming (CMD18) across sectors within a call
  (`chsd_stream`, `disk.cpp`); scanNext reads up to 4 KB and a frame keeps reading while the next call would end
  within 24 ms (worst frame seen 40 ms). Without the frame limit (debug `say X`) the scan takes about 6 s.
- `wait N` in chdrive scripts counts FRAMES in lockstep, not seconds: use `free N` for real-time waits.
- Debug: `say X` runs the whole scan printing `CARD <path> <why>` and `ITEM` lines; `STATE scan` has the timing.
- STARLIFT is not a scanner miss: the card holds 0.30j (stage select, no flash save); 0.38 scans as SLFT.
- v0.11: debug `say D <path>` dumps a card file as HEX lines (read-only), used to copy SAVES.BAK to the PC.
