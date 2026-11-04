# CHSaveMan: a save manager for CHGame

![SaveMan](docs/banner.png)

Every CHGame game keeps its save in two shared flash pages (0xF500 and 0xF600). This app shows what is in
them, which game each save belongs to, and lets you back them up to the SD card, restore them, or erase a page.

![GAMES LIST](docs/screenshot.png) ![The lander lifting a save](docs/lift.gif)

## Install

1. Copy `dist/SAVEMAN.CHG` (or the release's `SAVEMAN.CHG`) to the `GAMES` folder of the CHGame's SD card
   (FAT16 or FAT32).
2. On the board, pick SAVE MANAGER from the menu. Hold START for 3 s to go back to the menu.

**Safety, no warranty.** SaveMan writes the flash save pages every CHGame game shares. Back up first (BACK UP
PAGES...). Every restore and erase first writes the pages as they were to `SAVES.UND` (UNDO LAST CHANGE puts them
back) and every page write is read back, but the software comes with no warranty (GPL-3.0, sections 15-16).

**Status:** v0.19, board-tested. v0.18 ran on a real board with a real card: a real game's save round trip
(back up, erase, restore, the game loads it), a save and a power cycle, the NO CARD and CARD ERROR screens (the
driver's test switch, card in the slot) and a new game's SAVENAME.TXT append. v0.19 was measured on the board
with 78 games on the card: the scan takes 12.3 s (0.18: 36-37 s) and its worst frame is 31.1 ms (0.18: 47 ms;
30 fps needs 33.3 ms); GAMES LIST shows the games right after the scan. Still needs a card physically out of the
slot: NO CARD with an empty slot (the 250 ms CMD0 timeout); a non-FAT card (exFAT / NTFS) showing CARD NOT FAT32.
Image 49,952 B of 50,432, RAM 16,908 B.

## What it does

- **Main screen:** the two flash pages with the game, `#<counter>` (the save's age), and BAK (green: SAVES.BAK holds
  this record) or NEW (orange: not backed up yet); OLD for the spare copy, BAD for a bad CRC. A line shows what
  SAVES.BAK holds ("BAK  <A> + <B>"). At start, if a page holds a newest save the backup lacks, BACK UP NOW? is offered.
- **Back up** to `SAVES.BAK` (temp file, read back, renamed). A replaced backup is kept as `SAVES.BK1`. Page menus
  also have BACK UP THIS GAME (`SAVES/<CODE>.SAV`).
- **Page chooser:** BACK UP and RESTORE ask BOTH PAGES, ONLY A or ONLY B. A one-page backup keeps the other page of
  the old `SAVES.BAK`; a one-page restore leaves the other flash page alone.
- **Restore:** RESTORE PAGES lists the files on the card (SAVES.BAK, SAVES.BK1, SAVES.UND, every `SAVES/*.SAV`)
  with the games in each; pick one, then the page chooser and the question (what each page holds now and what it
  would hold, with LOSES warnings and version changes such as `V3 -> V2`). Every page write is read back.
- **Undo:** before every restore and erase the two pages are written to `SAVES.UND`; UNDO LAST CHANGE puts them back
  (undoing twice swaps back). A restore stops if the undo file cannot be written. An undo writes the pages it replaces
  to `SAVES.UN2` and replaces `SAVES.UND` only after its pages are in flash, so a power cut keeps both.
- **Erase a page:** asks first and warns when it is the only copy of a game's save.
- **FIND GAME NAMES:** scans `.CHG` files in `GAMES` and two levels of sub-folders (B stops it) and rewrites
  `GAMELIST.TXT`.
- **GAMES LIST:** UP / DOWN move (hold to repeat), LEFT / RIGHT page, SELECT cycles ALL / MISSING ONLY / SAVES ONLY
  / IGNORED ONLY, a short START press writes `GAMELIST.CSV`, A opens a game's details, B goes back. Rows are tagged
  A / B (that page holds the game now) and + (SAVES.BAK holds it).
- **Game details:** path, title, the name shown and where it came from, scan result (with candidates when UNSURE),
  flash page, backup status. Options: RESTORE ITS SAVE (from its `.SAV`), BACK UP ITS SAVE, IGNORE THIS GAME / SHOW
  IT AGAIN.
- **Names:** a built-in table (`Names.h`: public games only, CHGame's examples, CHix, StarLift, CHarth) and
  `SAVENAME.TXT` on the card, which wins and has no size cap. Every other game is learned on your own card: FIND
  GAME NAMES reads each game's title and appends `CODE=TITLE` (up to 32 new a scan; NAME LIST FULL means scan
  again for the rest).
- **Page A may hold game code:** a game over 50,432 bytes reaches into page A. SaveMan shows GAME CODE only when its own
  image covers page A; otherwise page A holds the tail of that game's code and reads UNKNOWN DATA. That is
  harmless (such a game never reads page A, and flashing it rewrites the page); BACK UP and RESTORE copy it as is.
- **If a flash write fails** the library turns saving off until restart (SAVING IS OFF); backup and restore are refused.

## The SD card

FAT16 or FAT32 only (FatFs is built without exFAT): a card that answers but is exFAT, NTFS or unformatted shows
CARD NOT FAT32, an empty slot NO CARD. Cards over 32 GB usually come exFAT: format them FAT32. Games are looked for
in `GAMES` and its sub-folders (the CHGame menu's layout).

## Controls

UP / DOWN choose, A picks, B goes back. Hold START for 3 s to return to the CHGame menu (the app counts
90 frames itself; the library's own count would take 6 s at 30 fps). In GAMES LIST a short START press writes the CSV.

## Files on the card

Root of the card unless stated.

- `SAVES.BAK`, `SAVES.BK1` (the backup before), `SAVES.UND` (the pages before the last restore or erase; `SAVES.UN2`
  the pages before the last undo): 528 bytes,
  little-endian: `"CHSVBK01"` (8), CRC-32 of the pages (u32), page count (u16, 2), set count (u16, 1), then page A
  (0xF500) and page B (0xF600), 256 bytes each. Temp files `.TMP` / `.UTM` (and `SAVES/<CODE>.TMP`) can be left by a
  power cut; they are read in place of the missing file and renamed by the next write.
  To use a `.BK1` by hand, rename it to `SAVES.BAK`.
- `SAVES/<CODE>.SAV`: one game's page, same header with a page count of 1 (272 bytes). `<CODE>` is the 4-character
  save code if it is all A-Z 0-9 _ -, else its 4 bytes in hex (`61622D63.SAV`). The previous one is `<CODE>.SV1`.
- `GAMELIST.TXT`: rewritten by each scan, one line per game: `PATH CODE TITLE`. PATH is under `GAMES` (`SUB/CHIX.CHG`);
  CODE is the 4-character save code, `-` for none, or `ABCD|WXYZ` (up to 3 candidates) when unsure; the title follows.
  Lines starting with `#` are comments; the last one is `# SKIPPED n FILES IN m DEEP FOLDERS` when folders deeper
  than `GAMES/X/Y` were skipped.
- `IGNORE.TXT`: one game per line, its path under `GAMES` or just its file name, any case; `#` comments. Ignored games
  are left out of the list counts (see IGNORED ONLY). The app appends and removes lines; a scan never touches it.
  Only the first 1 KB (about 50 games) is used.
- `SAVENAME.TXT`: lines `CODE=NAME`, `#` comments. Its names win over the built-in table; the app only appends.
  Use it to name a game the scan marks UNSURE.
- `GAMELIST.CSV`: written from GAMES LIST with START: header `file,folder,code,title,reason missing`, CRLF lines;
  reason is no save code found / unsure: several codes / ignored / both.

## Build and test

Needs: Arduino CLI with the CHGame board package (`CHGame:ch32v`, see the CHGame repository), Python 3 with
pyserial and Pillow, and on Windows Git Bash plus a gcc for the host tests (w64devkit: put its `bin` on PATH).

    ./cl build                                           flash and RAM
    ./cl run CHSaveMan/tools/scripts/smoke.txt out/smoke   screenshots
    ./cl test                                            host test: the board's card code + FatFs on a RAM disk
    ./cl check                                           everything: host test, scripts, all simulator tests, build
    python CHSaveMan/tools/tests/sim_save.py             one simulator test file (also extra_safety.py,
                                                         extra_features.py, sim_round3.py, sim_v019.py)
    python CHSaveMan/tools/gen_names.py <CHGame checkout>  Names.h from CHGame's examples + names_extra.txt
    python CHSaveMan/tools/gen_titlefont.py              TitleFont.h (SansBold12 cut to the title characters)
    python CHSaveMan/tools/make_pubcard.py               a public sim card for the README pictures (pictures.txt)

The simulator's card is a host file (`savm_sim.bak`, or `$SAVM_BAK`) and a folder (`$SAVM_CARD`).
`$SAVM_NOCARD` (`1`, `fat`, `err`) and `$SAVM_FAIL` (`io`, `crc`, `scan`, `append`, `read`, `cut`) fake card
faults; see `src/sd/sdio_sim.cpp`.

## Needs the save library's raw page calls

`save::rawPage()` and `save::rawWrite()` (and the flash-ownership change) are in the CHGame library's
`Save.h`/`Save.cpp`; `chgame/` here carries that modified version (see `chgame/PATCHES.md`).

## Licence and credits

GPL-3.0-or-later (see LICENSE): the SD card driver (Sd2Card, identical) and the FatFs glue (disk.cpp, given a
250 ms CMD0 timeout here) come from the CHSDtoSerial app in CHGame (Kevin Bates, bateske), itself derived from
William Greiman's sdfatlib (GPL-3.0); CHSaveMan/NOTICE lists exactly what was changed. FatFs is by ChaN (its own
licence, `CHSaveMan/src/fatfs/FatFs-LICENSE.txt`). Built on CHGame, Copyright 2026 Kevin Bates, Apache-2.0; the title font is CHGfx's SansBold12 (DejaVu Sans Bold).
Artwork (`docs/`) made by `tools/make_media.py` from STARLIFT's sprites.
