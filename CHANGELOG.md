# Changelog

## 0.20 (2026-11-05)
- GAMES LIST, short START: in ALL and MISSING ONLY it asks HIDE n GAMES? (games with no clear save code) and
  appends their paths to `IGNORE.TXT`, never changing existing lines. It stops at the 1 KB the app reads. In
  IGNORED ONLY it asks SHOW n GAMES? and keeps only the `#` lines (the file is removed when empty). In SAVES ONLY
  it writes `GAMELIST.CSV` (before, START wrote the CSV in every view).
- SHOW IT AGAIN (one game) drops blank lines and removes an empty `IGNORE.TXT`.
- Faster card jobs: a full mount only when the card may have changed, otherwise a boot-sector check.
- Flash fit: FAT12 is no longer mounted; smaller strcmp, title font and page menu.
- Board (78 games): HIDE ALL hid the 9 MISSING games, a rescan kept them hidden, SHOW ALL brought them back.
  Worst frames 32.3 ms (a pass), 31.2 ms (an append), 31.1 ms (SHOW ALL). Opening or scrolling GAMES LIST still
  takes 39.5 ms of card work (since 0.10).
- Image 50,428 B of 50,432, RAM about 16.9 KB.

## 0.19 (2026-11-04)
- Scan of 78 games in 12.3 s (was 36-37 s), worst frame 31.1 ms (was 47 ms), by reading sectors within a frame
  budget and skipping impossible save codes.
- An undo writes the pages it replaces to `SAVES.UN2`, and replaces `SAVES.UND` only afterwards, so a power cut
  keeps both.
- A failed back up or restore shows the card error on the main screen at once.
- `Names.h` holds public games only (CHGame examples, CHix, StarLift, CHarth); other games are learned into
  `SAVENAME.TXT` by a scan.
- Smaller title font (-0.5 KB). New simulator tests; image 49,952 B, RAM 16,908 B.

## 0.14 - 0.18 (2026-11-04, simulator and host tests)
- Restore from any two-page file with BOTH / ONLY A / ONLY B; RESTORE PAGES lists `SAVES.BAK`, `.BK1`, `.UND` and
  every `SAVES/*.SAV`.
- A replaced per-game save is kept as `<CODE>.SV1`; BACK UP ITS SAVE in game details.
- A bad `SAVES.BAK` is no longer renamed over a good `SAVES.BK1`. ERASE asks ERASE ANYWAY? when `SAVES.UND`
  cannot be written.
- Scan frame budget fixes; host test of the card code and FatFs on a RAM disk (`./cl test`).
- `GAMELIST.TXT` notes games with a bad header (`# NOT CHG1`).
- `dist/SAVEMAN.CHG` carries a menu picture.

## 0.13 and earlier
- 0.13: `SAVES.UND` and UNDO LAST CHANGE, `SAVES.BK1`, read-back of every page write, version changes in the
  restore question, per-game saves, GAMES LIST views, `IGNORE.TXT`, `GAMELIST.CSV`, BACK UP NOW? at start.
- 0.11: first runs on a real board and card (74 games).
- 0.10: the scan reads `GAMES` and two sub-folder levels; GAMES LIST screen; page chooser.
- 0.5 - 0.9: card scan for save codes (FIND GAME NAMES), backup and restore of both pages, `Names.h` plus
  `SAVENAME.TXT`, card error screens, page A erase.
