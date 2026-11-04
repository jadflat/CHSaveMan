# CHSaveMan notes

- **Pages:** 0xF500/0xF600 hold records `u32 magic; u8 version, flag; u16 seq; data; pad4; u32 crc32`. The size is not
  stored, so `inspect()` tries each size until the CRC matches. A page with a valid record of the same game as a newer
  record in the other page is the "OLD" spare. A record with no matching CRC and an unknown code shows OTHER FORMAT
  (likely another game's own format); a known code with a bad CRC shows BAD. Erasing anything but an empty page or a
  valid spare warns DATA WILL BE LOST (or LOSES <GAME> SAVE).
- **Saving off:** after a failed `rawWrite` the library is BROKEN until a restart and `rawPage()` is null: the app
  shows SAVING IS OFF (kind OFF), refuses backup/restore, and a restore names the page it could not write. The debug
  hook `F` makes the next write fail so the simulator can test this (the library's own BROKEN path is board-only).
- **Backup:** `SAVES.BAK` (528 B, see `sdio.h`), written as `.TMP`, read back, old file removed, renamed. If a SAVES.BAK
  (or a cut-off SAVES.TMP) already exists, BACK UP first asks REPLACE SAVES.BAK? (CONFIRM, NO by default, pending
  OP_BACKUP2); with none it goes straight through. Restore reads and checks the file first, then asks with the two
  pages' game names, then writes the stale page first.
- **Names:** lookup order is `SAVENAME.TXT` on the card (lines `CODE=NAME`, no cap on names) then the built-in
  `Names.h` (`tools/gen_names.py`: public games only, CHGame's examples and `tools/names_extra.txt`; every other
  game is learned by a scan). A scan learns at most 32 new codes (MAX_LEARNED; NAME LIST FULL: scan again). `sdio::namesRead` returns
  the whole lines of the first 1 KB, so a bigger file still works. The file is read at boot and at the start of each
  scan (never during one: that remounts the card) and is only appended to (`sdio::namesAppend`, leading newline,
  never rewritten): names found by a scan are appended after it ends (also when it ended with a card error); if that
  fails they stay waiting (`namesDirty`) and are written before the next scan. A leftover SAVENAME.TMP with no
  SAVENAME.TXT is renamed to the real name before the next write.
- **FIND GAME NAMES:** from `GAMES` and two levels of sub-folders (`GAMES/X/Y`; deeper folders are counted as
  skipped). 0.19: one step a frame first (SAVENAME.TXT's waiting names, its codes, the GAMES folder, GAMELIST.TXT),
  then sectors while `sdio::scanRoom` says they fit 18 ms of the frame; the new names are written in the two frames
  after SCAN DONE is shown (B stops the scan at once; `scanEnd` closes the folders). A .CHG has a title but no save code, so the code is recovered from the compiled payload: a
  RISC-V `lui rd,hi` + `addi rd2,rd,lo` pair whose constant is four printable ASCII bytes, scored by how it is used
  (`chgscan.h`: SURE/CALL/WORD; the record load may sit up to 12 bytes before the lui; the library's constants
  BGHC, EXFA, 3333, UUUU, CHGF are excluded). A code is learned only when it is the only one found or its SURE/CALL
  score is unique; other files count as UNSURE, codes already named as KNOWN. A title with no usable characters
  falls back to the 8.3 file name. Games built without such a constant (older library versions, games with
  their own save code) are not found.
- **Card errors:** NO CARD (no answer to CMD0 within 250 ms), CARD NOT FAT32 (FR_NO_FILESYSTEM: exFAT etc.),
  CARD ERROR (IO), NO GAMES FOLDER.
- **Tests:** `python CHSaveMan/tools/tests/sim_save.py` (one simulator per environment: `SAVM_NOCARD=1|fat`,
  `SAVM_FAIL=io|crc|scan` (crc affects the backup read-back only), card folders made in `tools/tests/out/`); host scanner test `tools/tests/host_scan.cpp`
  with a local list of real .CHG files (`scan_truth.txt`, not in the repository: it names private projects); `./cl run CHSaveMan/tools/scripts/scan.txt out/scan` needs
  `SAVM_CARD` pointing at a folder with `GAMES/*.CHG`. Debug hook writes (W/E/C) are queued and done after
  `gfx_wait()`: give them a frame before `G`.
- Tests cover: backup straight / REPLACE? NO / YES / only the .tmp present; SAVENAME.TXT over 1 KB (old content and
  lines past the cap kept, new names appended); B stops a long scan (SCAN STOPPED, 6 x 300 KB games); a 40 KB payload
  scan over many frames. A failed names write that leaves names dirty: `SAVM_FAIL=append` / `Z names` (0.13).
  0.18: `tools/tests/host_fatfs.cpp` runs `sdio_dev.cpp` and FatFs on a RAM disk (`./cl test`), and
  `tools/tests/sim_round3.py` covers the card error screens, power cuts in the undo and .SAV writes and a full round
  trip (see the root NOTES.md).
- **Board:** v0.11 and v0.18 ran on a real board with a real card; v0.19's scan was measured there (see the root
  NOTES.md). Not yet run there: NO CARD with the slot physically empty, a non-FAT card, the library's BROKEN state.
- Budget at 0.7: image 33.1 KB of 50.4 KB, static RAM 15.6 KB of 18.4 KB (0.6: 32.7 KB, 15.6 KB) (0.4: 28.9 KB, 14.7 KB).

## v0.12 to v0.14 (simulator only; see CHANGELOG.md)

- New card files: SAVES.BK1, SAVES.UND, SAVES/<CODE>.SAV/.SV1, IGNORE.TXT, GAMELIST.CSV; GAMELIST.TXT is now
  `PATH CODE TITLE` with `ABCD|WXYZ` for unsure games. Formats are in README.md.
- Debug hooks: Y (next write reads back wrong), U (power cut after the next page write), Z <mode> (sim fault at
  run time), O (boot step again). `SAVM_FAIL=names|cut`. `SAVM_OFFER=1` turns the boot BACK UP NOW? offer on in the sim.
- Names.h is packed and generated: run `tools/gen_names.py <CHGame checkout>`, never edit by hand.
- START exit: 90 frames counted by the app (`startExits = false`).
- Main menu index for tests (`goto`): 0,1 pages; 2 back up; 3 restore; 4 find names; 5 games list; 6 undo.
- `python CHSaveMan/tools/tests/extra_safety.py` (59 checks).
