# CHSd

**API reference: <https://bateske.github.io/CHGame/group__lib__chsd.html>**.

Read-only microSD access for CHGame games: a polled SPI-mode block driver
and a FAT16/FAT32 reader that turns a file name into runs of card blocks.
It is about 1.7 KB of flash and 24 B of RAM (plus the caller's run list),
and it shares SPI1 with CHGfx's panel.

It is the one SD library of the games that read text off the card:

| Game | File on the card | What it uses |
|---|---|---|
| CHWords | `WORDS.DIC` in the root (the full ENABLE word list) | `fat::open`, `fat::read` |
| CHWordWheel | `PHRASES.BNK` in the root (the phrase bank) | `fat::open`, `fat::read` |
| CHCrossword | `CHCW/*.CWD` (puzzle packs) | `mount`, `folder`, `match`, `runs`, `fat::read` |
| CHStlView (an app) | any `.STL` in any folder, read whole every frame | `mount`, `root`, `list`, `runs`, `sd::stream` |

## How a game uses it

    // Once, after gfx_wait() (the card shares SPI1 with the panel):
    fat::Run run[4];
    uint8_t nRuns = fat::open("WORDS   DIC", run, 4, gfx_chunkScratch());
    // ... then any block of the file, any time after gfx_wait():
    if (!fat::read(run, nRuns, k, gfx_chunkScratch())) { /* the card has gone */ }

- **Bus rule.** Call it only between `gfx_wait()` and the next flush. SPI1
  is handed back as CHGfx left it.
- **Buffer.** Every call borrows a 512 B, 4-byte-aligned buffer and
  clobbers it. CHGfx's chunk scratch is idle between `gfx_wait()` and the
  flush, so all three games use that.
- **After a failed read,** drop the card and call `sd::init()` (or
  `fat::open`) again before the next read. A late block can still be on its
  way, and only a reset clears it.
- **Names** are 8.3 short names as the directory stores them: 11 capitals,
  padded with spaces. `?` matches any character. Long names, volume labels,
  hidden entries and deleted entries are skipped.
- **Players' cards** must be FAT16 or FAT32, which is how cards up to 32 GB
  come. exFAT (64 GB and up) is reported as `E_EXFAT` so a game can say
  "reformat as FAT32". A file in more pieces than the game's run list is
  refused (`E_FRAG`). Copying it to a freshly formatted card always works.

**Browsing and streaming** (2026-10-02, for CHStlView): `fat::root()` and
`fat::list()` walk any folder, files and folders alike, through a
callback; `sd::stream()` reads a run of blocks with one CMD18 at 24 MHz,
each block landing by DMA (channels 2 and 3, borrowed between flushes)
while the caller's function works on the one before: about 0.17 ms a block
instead of ~1.5 ms. The games call neither, so their images are byte for
byte what they were.

The full API is in [src/Fat.h](src/Fat.h) and [src/SdSpi.h](src/SdSpi.h).

## Using it

CHSd is bundled in the CHGame board package's `libraries/` folder, so a
sketch only needs `#include <Fat.h>` (and `<SdSpi.h>` for raw blocks). In
this repository `tools/device.py` passes the folder with `--library`.
CHWords, CHCrossword and CHWordWheel use it this way; they used to carry
generated copies, which are gone.

In the simulator (`tools/chsim/chsim.py`) a sketch that includes CHSd is
built with `src/Fat.cpp` as it is and `host/sd_host.cpp` in place of the
SPI driver: the file named by `$CHSD_CARD` is the card (a `*.img` as a
whole card, any other file on a FAT16 card made for it by `host/VCard.h`).

To change the library, edit it here, then run (from this folder):

    python tests/run_tests.py

Then run `chgame check` in each of the three games, which also checks that
both save pages still fit.

## Testing

- `tests/run_tests.py` runs `src/Fat.cpp` on the PC against FAT16 and FAT32
  images built by `tools/fatimg.py`, whose layout is the ground truth. The
  images cover:
  - with and without a partition table, and the partition in slot 3;
  - 2 KB and 4 KB clusters, one FAT or two;
  - long names and decoy entries;
  - files and the FAT32 root directory in pieces;
  - a hidden file, a same-named file in another folder, and empty and
    one-byte files;
  - looped, cut-short and over-long FAT chains;
  - exFAT, a blank card, and a FAT12-sized volume.

  It also pulls the card after every possible number of reads. `--quick`
  leaves out the 34 MB FAT32 images.
- It also builds `src/SdSpi.cpp` (its `init()` and `read()`) against a model
  of a card in SPI mode, `tests/test_spi.cpp`, behind stand-in registers
  (`tests/spi/Arduino.h`; a 32-bit build, which zig makes on Windows and
  x86-64 Linux): SDHC, SDSC v2 and v1, no card, the SPI handed back as
  CHGfx left it, identification under 400 kHz. The model keeps CRC
  checking on across CMD0 and an MCU reset, as a card CHSDtoUSB has used
  may, so the driver must turn it off itself (CMD59, 2026-10-07).
- The simulators' card (`host/sd_host.cpp`) serves a `.img` file as the
  whole card. Any other file (`sdcard/WORDS.DIC`, `sdcard/PHRASES.BNK`) goes onto a
  pretend FAT16 card built on the fly (`host/VCard.h`), in two pieces with a
  gap between them. So every simulator session runs the real FAT code,
  including a step from one run to the next.
- **Not yet run on a board.** None of the three games has read a real card
  yet. The first device session should check: a FAT32 SDHC card, a small
  FAT16 card, an exFAT card (the message), pulling the card mid-game, and
  how long `fat::open` and one `fat::read` take.

## Review of the three copies (2026-10-01)

**Before.** CHWords cut HypeRunner's driver down to polled, read-only
single-block reads. CHWordWheel took that cut unchanged; its `src/sd` was
byte-identical to CHWords'. CHCrossword took a snapshot of it and went its
own way:

| | CHWords / CHWordWheel | CHCrossword |
|---|---|---|
| Token timeout | 300 ms | **1 s**: some cards take 300-770 ms for a block's first read after power-up |
| FAT lookups | root directory only | **folder + `?` patterns + skip**, hidden files filtered (macOS `._` files would otherwise match `*.CWD`) |
| `Fat.cpp` on the PC | compiled out; the sim passed the raw file as the card, so the FAT code **never ran off the board** | compiled in, host-tested on FAT16/FAT32 images |
| Extent to LBA mapping | its own loop in each game | its own loop |

**Merged into CHSd:**

- CHCrossword's lookup is used in all three, and its 1 s timeout too.
- The run-mapping loop the three games each had is now `fat::read`. The
  init + mount + find + runs sequence two of them repeated is now
  `fat::open`.
- The simulators of CHWords and CHWordWheel, and CHWordWheel's bank tests,
  now go through the FAT code (VCard). The library has its own test suite,
  which all three share.

**Smaller, measured on the release images (oslto, Upload only):**

| Game | Before | After | Change |
|---|---|---|---|
| CHWords | 50,416 B | 50,396 B | −20 B |
| CHCrossword | 50,400 B | 50,300 B | −100 B |
| CHWordWheel | 50,408 B | 50,312 B | −96 B |

Static RAM is unchanged in all three. Where the bytes came from:

- **Word-sized loop counters.** A `uint8_t` counter costs a mask on every
  pass.
- **One timeout constant for both waits.** With 300 ms for both, GCC had
  folded it into `wait()`. Raising only the token wait to 1 s cost 16 B, so
  both waits now use 1 s. A read-only driver never leaves the card busy, so
  the longer busy wait costs nothing in practice.
- **One attribute test instead of two.** Hidden files, labels, long-name
  parts and file-versus-folder are all rejected by one masked compare,
  which costs nothing over the old label check.
- **`fat::open` returns only a run count.** Passing the error code through
  cost about 40 B in the games that never show it.
- **Extra flags on `SdSpi.cpp` only.** `no-jump-tables` and
  `no-guess-branch-probability` save up to 28 B there, but cost up to 88 B
  on `Fat.cpp`.

**Considered and not done:**

- **A faster clock (24 MHz), pipelined SPI or DMA for `read()`.** (A
  streaming reader that does all three, for a sketch that reads whole files
  every frame, is `sd::stream()`, which the games do not use.) A block is about
  0.5 ms of polled bytes, and the card's own access time (0.1-1 ms or more)
  comes on top, so these would save under 0.3 ms per block. Nobody would
  notice that in these games. There is also no CRC on the wire to catch a
  marginal 24 MHz signal. A pipelined loop overruns if an interrupt lands
  mid-block. DMA would need CHGfx's channel 3. HypeRunner, which streams,
  does all of this with CRCs and a step-down; word games do not need it.
- **A block cache.** It would need its own 512 B of RAM, since the chunk
  scratch is redrawn every frame. CHCrossword has under 1 KB of RAM left,
  and no game reads the same block twice in a frame.
- **CMD12 after a late token.** It costs about 16 B. Every game already
  drops the card on a failed read and re-initialises it (CMD0) before
  reading again, so the late block can never be taken for an answer.

## License

MIT (LICENSE). From HypeRunner's clean-room driver; see NOTICE.
