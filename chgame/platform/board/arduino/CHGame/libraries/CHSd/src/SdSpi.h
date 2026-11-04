// SdSpi - a read-only SPI-mode microSD block driver (CHSd, MIT: see NOTICE).

/// @file
/// @brief The SD card's blocks: identify the card, read one 512-byte block
/// or stream a run of them.

/// @defgroup chsd_sd SD card blocks
/// @ingroup lib_chsd
/// @brief A read-only SPI-mode microSD block driver: `sd::init()`,
/// `sd::read()` and `sd::stream()`.
///
/// HypeRunner's clean-room driver cut down to what reading a game's data
/// file needs: identify the card, read one 512-byte block. No writing, no
/// CRC (the games' files check their own blocks or records), and DMA only
/// in `stream()`, for a sketch that reads whole files every frame. Most
/// sketches never call these directly: @ref chsd_fat "fat::open()" and
/// `fat::read()` do it for them.
///
/// The card is a block device addressed by LBA (512-byte block number)
/// whatever its size: SDSC, SDHC and SDXC cards all work.
///
/// @warning **Bus rule.** SPI1 is shared with the LCD. Call these only after
/// `gfx_wait()`, before the next flush. SPI1 is handed back exactly as
/// CHGfx left it.
///
/// @note After a read fails, call `sd::init()` before reading again: a
/// block whose token came late may still be on its way, and `init()`'s CMD0
/// is what clears it.
/// @{
#pragma once
#include <stdint.h>

/// @brief The SD card's SPI-mode block driver (CHSd).
namespace sd {

/// @brief Wakes the card and identifies it (SD v1, SDSC v2, SDHC/SDXC).
///
/// Runs at 187.5 kHz with the card's CS (PB11) low, then sets 12 MHz for
/// `read()`. Takes up to about a second with a card that is slow to come
/// ready; with no card it fails in about 15 ms (MISO is pulled up, so an
/// empty slot reads 0xFF). Call it again after any failed read, or after
/// the card may have been swapped.
/// @return `true` if a card answered and is ready; `false`: no card, or not
///         one this can read.
bool init();                                // false: no card, or not one this can read

/// @brief Reads one 512-byte block (CMD17), polled at 12 MHz.
///
/// About 0.5 ms of bytes plus the card's own access time (0.1-1 ms, up to
/// ~0.8 s for a block's first read after power-up); ~1.5 ms in all is
/// typical. The block's CRC is not checked.
/// @param lba  the block number on the card (0 = the card's first block,
///             whatever the card type).
/// @param dst  where the 512 bytes go.
/// @return `true` if the block arrived; `false`: the card did not deliver
///         (call `init()` before the next read).
bool read(uint32_t lba, uint8_t *dst);      // one block; false: the card did not deliver

/// @brief What `stream()` calls with each block, in order.
///
/// It runs while the next block arrives by DMA, so it must keep off SPI1
/// and off the other of `stream()`'s two buffers, and must not read the
/// card.
/// @param block  the block's 512 bytes (in `buf0` or `buf1`), valid until
///               the callback returns.
/// @param ctx    the `ctx` given to `stream()`.
typedef void (*BlockFn)(const uint8_t *block, void *ctx);

/// @brief Reads `n` consecutive blocks with one multi-block read (CMD18),
/// handing each to `fn` while the next one arrives.
///
/// For a sketch that reads a file over and over (CHStlView reads a whole
/// model every frame). The blocks come at 24 MHz by DMA into `buf0` and
/// `buf1` in turn while `fn(block, ctx)` works on the one before; `fn`
/// gets each block once, in order. Costs about 0.6 ms of card latency per
/// call plus 0.17 ms per block, against ~1.5 ms per block for `read()`.
/// No CRC is checked (a block garbled on the wire reaches `fn` as it
/// came). DMA1 channels 2 and 3 are borrowed and left off: CHGfx sets
/// channel 3 up afresh for every flush. The games never call it, so it is
/// not in their images.
/// @param lba   the first block's number on the card.
/// @param n     how many blocks to read; 0 does nothing and returns `true`.
/// @param buf0  a 512-byte buffer for the even blocks (0, 2, 4 ...).
/// @param buf1  a 512-byte buffer for the odd blocks; not the same as
///              `buf0`.
/// @param fn    called once per block, in order (see @ref BlockFn).
/// @param ctx   passed to `fn` unchanged; may be `nullptr`.
/// @return `true` if every block was delivered; `false`: the card stopped
///         delivering (call `init()` before the next read). Some of the
///         blocks may have reached `fn` before the failure.
bool stream(uint32_t lba, uint32_t n, uint8_t *buf0, uint8_t *buf1, BlockFn fn, void *ctx);

}  // namespace sd

/// @}
