# CHSaveMan on the MiSTer core: test card handoff

Status: image built and checked with an independent Python FAT32 reader. Not run on the MiSTer core or the sim.

## The image: dist/chsaveman_card.img
- 64 MB, MBR + one FAT32 partition (LBA 2048), label CHSAVEMAN, 512 B clusters, 8.3 names only, no LFN.
- GAMES\: SAVEMAN.CHG plus 11 real games (long names cut to 8.3, e.g. ALIENBRE.CHG, GETOUTDU.CHG) and GAMES\MORE\CHFIGHT.CHG.
- Codes expected from scan_truth.txt: CABD CAWD CASC CBAS BECK CBMB CDTB CDRL GOTD FEAT CHFT; Celeste2 has none (-).
- All 13 files match their source SHA-256 (list in out/chsaveman_card.img.manifest). Save pages live in flash, so none are seeded.
- Rebuild: python tools/mkcard.py [folders or .CHG files] [--no-saveman] [--max N] [--size-mb N].

## How the sim loads a card
- sim/tb_savm.cpp calls sdcard_open(path, ...) from ctest/src/sdcard.c, which takes a folder (builds FAT32 in memory, syncs changes back at the end) or an image file.
- So run `savm game.chg seconds cardC cardR script.txt outdir` with the .img for cardC and a copy for cardR. Not tried here.
- On real hardware: write the .img to an SD card (not exFAT) and pick SAVEMAN in the menu.

## Check after BACK UP
- SAVES.BAK in the card root is exactly 528 B, starts with tag CHSVBK01, and its crc32 matches the app.

## Check after FIND GAME NAMES
- SAVENAME.TXT in the card root lists code -> game name. Compare with the codes above (tools/tests/scan_truth.txt).
- CHFIGHT (code CHFT) is in the sub-folder, so it shows whether the scan descends into GAMES\MORE.

## Known issue
- The stock sim/RTL sdspi sends 0xFF 0xFF as the data CRC16, but the app driver checks CRC16 (CMD59 on).
- Result: NO CARD appears until a CRC16-CCITT fix exists in the sdspi (scratchpad copies in mister/emu and the core).
