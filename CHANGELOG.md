# Changelog

## 0.18 (2026-11-04, simulator and host tests only)
- Scan frame time: the frame's card budget guessed the next call's time from the last call, so a slow call could
  start late (board 0.11: 40 ms frames with a 24 ms budget; the host model puts 0.17's loop at 32-39 ms). Now the
  guess is the slowest recent call (decaying 1/16 a call), SCAN_BUDGET 12 ms, SCAN_STEP 2 KB (was 4 KB), and a
  scanNext call also stops after opening a folder, a .CHG with a bad header or 16 folder entries. Modelled worst
  scan frame 25.5 ms (30.7 ms with a sudden slow call); needs a board run to confirm.
- A per-game save cut off by a power cut between the old `.SAV` becoming `.SV1` and the rename (only
  `SAVES/<CODE>.TMP` left) was missing from the RESTORE list; it is now listed as `<CODE>.SAV`.
- New tests: `tools/tests/host_fatfs.cpp` (the board's card code and FatFs on a RAM disk, FAT16 and FAT32: mount
  results, SAVES folder, .BK1/.SV1 renames, power cuts after every sector write, failed writes, the scan, a frame
  model; 159 checks, run by `./cl test`) and `tools/tests/sim_round3.py` (126 checks). `./cl check` now runs all
  the simulator test files. Simulator switches: `SAVM_NOCARD=err` (a card that fails init), `Z nocard|nofat|cardfail`.
- `dist/SAVEMAN.CHG` now carries the menu picture (`docs/cart.png`, packed with the CHGame repo's chgpack, which has
  `chcart`): payload 50,308 B, crc 0x24660CD5. Image 50,308 B (124 B free), RAM 16,892 B; `./cl check` ALL GOOD.

## 0.17 (2026-11-04, simulator only)
- UNKNOWN DATA pages show their first 16 bytes (hex) in the page menu; the hint says A: SEE ITS FIRST BYTES.
- GAMELIST.TXT ends with `# NOT CHG1: n FILES` when .CHG files with a bad header were passed over.
- The BAK line is read again when the main screen comes back after a card error.
- -488 B by keeping the small drawing helpers out of line. Image 50,088 B (344 B free), RAM 16,876 B;
  sim_save 183, extra_safety 59, extra_features 23, cl check ALL GOOD. Round-3 list: out/improvements3.md.

## 0.16 (2026-11-04, simulator only)
- One-page restores from any two-page file: RESTORE -> SAVES.BAK / .BK1 / .UND -> BOTH / ONLY A / ONLY B. The chooser
  now shows what the file holds for each page (game name), the file name under it; the question is RESTORE PAGE A?/B?
  (also for SAVES.UND); the other flash page is never written; SAVES.UND is still written first.
- GAMES LIST details lose RESTORE ITS SAVE (use RESTORE's file list) to make room.
- Image 50,316 B (116 B free), RAM 16,872 B; sim_save 176, extra_safety 59, extra_features 23, cl check ALL GOOD.

## 0.15 (2026-11-04, simulator only)
- A SAVES.BAK that fails its tag/CRC check is no longer renamed to SAVES.BK1 when a new backup replaces it: the good
  SAVES.BK1 stays (tester F1).
- ERASE of a page holding a save when SAVES.UND cannot be written now asks ERASE ANYWAY? (NO by default) (tester F2).
- Image 50,360 B (72 B free), RAM 16,872 B; sim_save 171, extra_safety 59, extra_features 23 checks, cl check ALL GOOD.

## 0.14 (2026-11-04, simulator only)
- RESTORE PAGES lists SAVES.BAK, SAVES.BK1, SAVES.UND and each SAVES/*.SAV with their games; pick one, then the chooser.
- Replacing a game's .SAV keeps the old one as `<CODE>.SV1`. Erase says NO UNDO when no card was found at boot.
- GAMES LIST details gain BACK UP ITS SAVE. Names.h packed (-336 B): regenerate it with gen_names.py.
- Image 49,976 B (456 B free), RAM 16,872 B; 166 + 59 checks. A debug build no longer fits.

## 0.13 (simulator only)
- Safety: SAVES.UND before every restore/erase and UNDO LAST CHANGE; SAVES.BK1 for the replaced backup; every page
  write is read back; restore shows version changes (V3 -> V2) and names another format's owner.
- Per-game saves `SAVES/<CODE>.SAV` (back up from the page menu or game details, restore into the right page).
- GAMES LIST: views (ALL / MISSING ONLY / SAVES ONLY / IGNORED ONLY), A/B/+ tags, key repeat, game details, IGNORE.TXT,
  GAMELIST.CSV on START. Main screen shows #counter, BAK/NEW per page and what SAVES.BAK holds; boot offer BACK UP NOW?.
- GAMELIST.TXT is `PATH CODE TITLE` with candidates for UNSURE and a `# SKIPPED` line; unsure ties break on known
  codes; no 32-name cap on SAVENAME.TXT lookups. Scan frame budget 16 ms.
- START exit counts 3 s itself (was 6 s at 30 fps). Power-cut and failed-append tests added.

## 0.12 (2026-11-04)
- Image 37,120 B, RAM 16,184 B, 101 simulator checks. Docs brought up to date; pre-push scrub of local paths.

## 0.11
- First runs on a real board (real card, 74 games). Debug `say D <path>` dumps a card file as hex.
- The scan streams sectors (CMD18) within a 24 ms frame budget; 14.6 s for 74 games.

## 0.10
- The scan reads GAMES and two levels of sub-folders and writes `GAMELIST.TXT` (`FILE.CHG CODE NAME`).
- GAMES LIST screen pages that file (UP / DOWN, LEFT / RIGHT, B).
- BACK UP and RESTORE ask BOTH PAGES / ONLY A / ONLY B; a one-page backup keeps the other page of the old file.

## 0.9 (review of 0.8)
- The restore question shows what each page holds now and what the backup puts there (SAME, OLD), with an orange
  LOSES warning. `._` and tiny files no longer inflate the game count. All of SAVENAME.TXT is searched.
  A failed SAVES.BAK read gives CARD ERROR, not BAD BACKUP. Image 33,876 B.

## 0.8
- Page A erase; MiSTer simulation runs; first local commit.

## 0.7 / 0.6
- Built-in name table (`Names.h`) plus `SAVENAME.TXT` (appended by the scan), UNSURE handling, REPLACE SAVES.BAK?
  question, card error screens (NO CARD, CARD NOT FAT32, CARD ERROR). Image 33.1 KB at 0.7.

## 0.5
- Card scan of `.CHG` files for save codes (FIND GAME NAMES), backup and restore of both pages.
