// Fat: read-only FAT16/FAT32 on top of sd::read (CHSd; clean-room, MIT,
// from HypeRunner: see NOTICE).

/// @file
/// @brief FAT16/FAT32 files on the SD card: find a file by its 8.3 name and
/// read its blocks.

/// @defgroup chsd_fat FAT16/FAT32 files
/// @ingroup lib_chsd
/// @brief A read-only FAT16/FAT32 reader on top of @ref chsd_sd "sd::read()":
/// a file name in, the file's runs of card blocks out.
///
/// Written from the Microsoft FAT specification (BPB fields, FAT type from
/// the cluster count, 8.3 directory entries, end-of-chain marks) and the
/// MBR partition table layout. It only turns a file name into LBA runs:
/// after that a file is read as raw card blocks through its run list, so
/// there is no FAT write code, no cache and no file object. Every call
/// borrows the caller's 512 B buffer (4-byte aligned) and clobbers it; the
/// volume state is 24 B. What a game does not call is left out of its image
/// by the linker.
///
/// **Names** are 8.3 short names as the directory stores them: 11
/// characters, capitals, the name and the extension padded with spaces
/// (`"WORDS   DIC"` for `WORDS.DIC`), and `?` stands for any character.
/// Long-name entries, volume labels and hidden entries are skipped; a file
/// never matches a folder name, nor a folder a file name. Files are looked
/// for in the root directory or in a folder of it.
///
/// @code
/// // Once, after gfx_wait() (the card shares SPI1 with the panel):
/// fat::Run run[4];
/// uint8_t nRuns = fat::open("WORDS   DIC", run, 4, gfx_chunkScratch());
/// // ... then any block of the file, any time after gfx_wait():
/// if (!fat::read(run, nRuns, k, gfx_chunkScratch())) { /* the card has gone */ }
/// @endcode
///
/// @warning **Bus rule.** Every call reads the card over SPI1, which the
/// panel shares: call them only between `gfx_wait()` and the next flush.
/// CHGfx's `gfx_chunkScratch()` is idle at exactly that time and makes a
/// good buffer.
/// @{
#pragma once
#include <stdint.h>

/// @brief The FAT16/FAT32 reader (CHSd).
namespace fat {

/// @brief What the functions that return an `int8_t` report: `OK` (0) or a
/// negative error.
enum Err : int8_t {
    OK = 0,                  ///< success
    E_READ = -1,             ///< the card did not deliver a block
    E_NOFS = -10,            ///< no FAT16/FAT32 volume: no partition, bad BPB, FAT12
    E_EXFAT = -11,           ///< exFAT (or NTFS) volume: reformat the card as FAT32
    E_NOTFOUND = -12,        ///< no such file or directory (or not mounted)
    E_FRAG = -13,            ///< more runs than the caller has room for
    E_CHAIN = -14,           ///< cluster chain broken, looped, or not the file's length
};

/// @brief A run of consecutive card blocks holding part of a file (see
/// runs()).
struct Run { uint32_t lba, blocks; };
/// @var fat::Run::lba
/// The run's first block on the card (an LBA, as sd::read() takes it).
/// @var fat::Run::blocks
/// How many 512-byte blocks the run holds (at least 1).

/// @brief A file or folder found in a directory: where it starts and how
/// long it is.
struct File { uint32_t cluster, size; };      // first cluster, size in bytes
/// @var fat::File::cluster
/// Its first cluster (0 for an empty file; for root(), the FAT16 root's 0
/// or the FAT32 root's cluster).
/// @var fat::File::size
/// Its size in bytes (0 for a folder).

/// @brief Finds the FAT volume on the card and remembers where its parts
/// are.
///
/// Looks for a superfloppy boot sector at LBA 0, else the first MBR
/// partition of type 01/04/06/0B/0C/0E. An exFAT boot sector at LBA 0, or
/// an MBR with a type 07 partition and no FAT one, is `E_EXFAT`. Call
/// `sd::init()` first; everything else here needs a mounted volume.
/// @param buf  a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return `OK`, `E_READ`, `E_NOFS` (also FAT12, and a blank card) or
///         `E_EXFAT`.
int8_t mount(uint8_t *buf);

/// @brief Finds a file in the root directory.
/// @param name  its 11-character 8.3 name (`"WORDS   DIC"`); `?` matches
///              any character.
/// @param f     receives the file's first cluster and size.
/// @param buf   a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return `OK`, `E_NOTFOUND` (also when not mounted) or `E_READ`.
int8_t find(const char *name, File &f, uint8_t *buf);

/// @brief Finds a folder in the root directory, for match() and list().
/// @param name  its 11-character 8.3 name, padded with spaces
///              (`"CHCW       "`); `?` matches any character.
/// @param dir   receives the folder.
/// @param buf   a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return `OK`, `E_NOTFOUND` (also when not mounted) or `E_READ`.
int8_t folder(const char *name, File &dir, uint8_t *buf);

/// @brief Finds the (`skip` + 1)th file in a folder whose name fits a
/// pattern.
///
/// Call it with `skip` = 0, 1, 2 ... to go through every match, in
/// directory order.
/// @param dir      the folder, from folder() or root().
/// @param pattern  an 11-character 8.3 name in which `?` matches any
///                 character (`"????????CWD"`: every `.CWD` file).
/// @param skip     how many matching files to pass over first.
/// @param f        receives the file's first cluster and size.
/// @param nameOut  receives its 11-character name (no NUL); `nullptr` if
///                 not wanted.
/// @param buf      a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return `OK`; `E_NOTFOUND`: there are no more (or not mounted);
///         `E_READ`.
int8_t match(const File &dir, const char *pattern, uint8_t skip, File &f, char *nameOut, uint8_t *buf);

/// @brief The root directory as a folder, for match() and list().
/// @param dir  receives the root directory.
/// @return `OK`; `E_NOTFOUND`: not mounted.
int8_t root(File &dir);

/// @brief What list() calls for each entry of a directory.
/// @param name   the entry's 11-character 8.3 name (no NUL), valid only
///               during the call.
/// @param f      its first cluster and size.
/// @param isDir  `true` for a folder, `false` for a file.
/// @param ctx    the `ctx` given to list().
/// @return `true` to go on to the next entry, `false` to stop the walk.
typedef bool (*ListFn)(const char *name, const File &f, bool isDir, void *ctx);

/// @brief Walks a directory, handing every file and folder in it to `fn`,
/// in directory order.
///
/// `fn` gets each one's 11-character short name (no NUL), its first
/// cluster and size, and whether it is a folder; returning `false` stops
/// the walk. `.`, `..`, deleted entries, volume labels, long-name parts and
/// hidden entries are left out. (For a browser: CHStlView.)
/// @param dir  the directory, from root() or folder().
/// @param fn   called once per entry (see @ref ListFn); it must not touch
///             `buf` (the directory sector is in it) nor read the card.
/// @param ctx  passed to `fn` unchanged; may be `nullptr`.
/// @param buf  a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return `OK` (also when `fn` stopped the walk), `E_NOTFOUND` (not
///         mounted) or `E_READ`.
int8_t list(const File &dir, ListFn fn, void *ctx, uint8_t *buf);

/// @brief Walks a file's cluster chain once and returns its extents: the
/// runs of consecutive card blocks that hold it.
///
/// The last run is trimmed to the file's size, and a file of 0 bytes has 0
/// runs. The walk is bounded by the file's size and the chain must end
/// right there, so a looped or truncated FAT gives `E_CHAIN` instead of a
/// hang.
/// @param f        the file, from find() or match().
/// @param out      receives the runs.
/// @param maxRuns  how many runs `out` has room for (at most 127 are used).
/// @param buf      a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return the number of runs (0 or more), or `E_FRAG` (more runs than
///         `maxRuns`: copying the file to a freshly formatted card always
///         helps), `E_CHAIN` or `E_READ`.
int8_t runs(const File &f, Run *out, uint8_t maxRuns, uint8_t *buf);

/// @brief The usual way in: `sd::init()`, mount(), find() and runs() in one
/// go.
///
/// A game that tells the player why there is no file (no card, exFAT ...)
/// calls the steps itself: passing the reason on would cost every game some
/// 40 B of flash.
/// @param name     the file's 11-character 8.3 name in the root directory
///                 (`"WORDS   DIC"`).
/// @param out      receives the file's runs.
/// @param maxRuns  how many runs `out` has room for.
/// @param buf      a 512-byte, 4-byte-aligned buffer; clobbered.
/// @return the number of runs; 0 if any step failed or the file is empty.
uint8_t open(const char *name, Run *out, uint8_t maxRuns, uint8_t *buf);

/// @brief Reads block `k` of a file through its runs.
/// @param run   the file's runs, from open() or runs().
/// @param nRuns how many there are.
/// @param k     the block of the file (0 = its first 512 bytes).
/// @param dst   where the 512 bytes go (any buffer of 512 bytes).
/// @return `true` if the block was read; `false`: past the end of the
///         file, or the card did not deliver it (then `sd::init()`, or
///         open(), before the next read).
bool read(const Run *run, uint32_t nRuns, uint32_t k, uint8_t *dst);

}  // namespace fat

/// @}
