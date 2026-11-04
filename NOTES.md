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
5. `SAVENAME.TXT` is only ever appended to; lines the app does not use are kept, and every name in it is used.
   A scan learns at most 32 new codes (NAME LIST FULL: scan again for the rest).

## Tests

`./cl check` runs everything (ALL GOOD at 0.19): the host test, the scripts, the five simulator test files
(`tools/game.py` lists them) and the device build. On their own:

- `./cl test`: `tools/tests/host_fatfs.cpp`, 193 checks. The board's card code (`src/sd/sdio_dev.cpp`) and FatFs
  with the board's `ffconf.h`, on a RAM disk formatted FAT16 (2 KB and 32 KB clusters) and FAT32 (MBR, 512 B
  clusters): NO_CARD / IO / NOT_FAT mounts; SAVES.BAK / .BK1 / .UND, the SAVES folder, .SAV / .SV1 renames;
  the disk image after every sector write of a backup, an undo, a .SAV and a SAVENAME.TXT append (each reads as
  the old or the new file, the lone .TMP case included, and the next write recovers); a failing sector write at
  every point (IO, nothing lost); the GAMES scan (every code, codes on chunk edges, deep / junk / bad files, a
  remount and a pulled card mid-scan); a frame-time model of the app's scan loop (0.19: `scanRoom`, no scanNext
  call writes a sector, GAMELIST.TXT written line by line).
- `python CHSaveMan/tools/tests/sim_save.py` (183), `extra_safety.py` (59), `extra_features.py` (23),
  `sim_round3.py` (126: every screen with NO CARD / CARD NOT FAT32 / CARD ERROR at boot and at run time, a
  failed SAVENAME.TXT append written once later, a multi-frame scan naming every game, power cuts in the undo
  and .SAV writes, read-back failures, a full round trip byte for byte), `sim_v019.py` (an undo cut by a power cut
  keeps SAVES.UND, NO CARD / CARD ERROR on the BAK line and its re-read, the RESTORE FROM row, GAMES LIST after a
  multi-frame scan).
- Simulator faults (`src/sd/sdio_sim.cpp`): `SAVM_NOCARD=1|fat|err`, `SAVM_FAIL=io|crc|scan|append|read|cut`,
  at run time `say Z <mode>` (also `nocard`, `nofat`, `cardfail`; `Z -` back to `$SAVM_FAIL`).

The app ran on a real board at v0.11, v0.18 and v0.19.

## Scan frame time (0.19, measured on the board)

A frame is the card work, the drawing and the wait for the last panel flush (SPI1 is shared, so card reads cannot
overlap it): about 7 ms without card work. 0.18 read 2 KB a scanNext call and guessed the next call's time: 1.6
calls a frame, a 78-game scan in 36-37 s, worst frame 36-37 ms (one call 24.8 ms; a second run had one at 32.7 ms,
a cluster boundary: FAT read and new CMD18, the card stalling). Board time per 512-byte sector: read 0.37 ms, the
scanner 0.9 ms (0.19's prefilter in `chgscan.h` halves that: a lui is looked at only when its top two bytes can
still be printable). 0.19: `sdio::scanRoom(us)` decides per sector whether more fits in SCAN_BUDGET (18 ms) of the
frame; a file or folder step starts only with SCAN_OPEN_US (11 ms) left (else WAIT); a GAMELIST.TXT line is written
by listAdd itself (FF_FS_TINY: `f_flushwin`), not by the next scan step; the start (names, SAVENAME.TXT codes,
GAMES, GAMELIST.TXT) and the end (GAMELIST.TXT closed; the names searched, then appended) are one step a frame.
Board, 78 games: scan 10.9 s at a 20 ms budget, 12.3 s with 32 new names; the numbers per frame are in CHANGELOG.md.

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
