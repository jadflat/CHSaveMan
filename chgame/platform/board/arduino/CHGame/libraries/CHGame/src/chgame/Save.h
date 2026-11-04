// MODIFIED for CHSaveMan (Apache-2.0 section 4(b)), against upstream bateske/CHGame e1f8d05: save::rawPage() and
// save::rawWrite() (whole-page access for a save manager), the flash-ownership rule (status(), BLOCKED,
// allowOverwrite()), save backends with slots (useBackend(), slots()). See chgame/PATCHES.md.
/// @file Save.h
/// @brief Saving: a record in flash that survives power cycles and re-uploads.
///
/// CHGame has no EEPROM. The bootloader erases only the flash pages a new
/// sketch occupies, so the last two pages of the application region
/// (0xF500, 0xF600) survive. A record is a header, the game's own data and a
/// CRC; saves take turns between the pages, so a power cut in the middle of
/// one loses only that save. Every game shares the two pages and tells its
/// records apart by a magic number of its own (each game needs a different
/// one). If the sketch grows into the pages, saving switches itself off
/// rather than overwrite code: an image up to 50,432 B keeps both pages, up
/// to 50,688 B one.
#pragma once
#include <stdint.h>

/// @defgroup chgame_save Saving
/// @ingroup lib_chgame
/// @brief save::: a game's options and progress in flash, with a CRC, kept
///        across power cycles and re-uploads.
///
/// @code
/// struct SaveData { Options opt; Stats stats; };     // up to 244 bytes
/// const uint32_t MAGIC = save::magic("CHXX");
///
/// SaveData d;
/// if (save::load(MAGIC, 1, d)) { ... }                // at start-up
/// save::store(MAGIC, 1, d);                           // after gfx_wait()
/// @endcode
///
/// Change the version when SaveData changes shape: old records are then
/// ignored instead of misread. A flag byte travels in the header (most of
/// the casino games mark "a game in progress" with it).
///
/// The page is built in CHGfx's chunk scratch, so store() must run between
/// gfx_wait() and the next flush. read() points straight into flash: a game
/// can take just the part it needs.
///
/// @note Load once at start-up, before the first store: the read remembers
/// the newest record's sequence number, and the next save follows it.
/// @{

/// @brief Saved records: load, store, and the raw read/write underneath.
namespace save {

/// @brief The most data a record holds: a 256-byte page less the header and CRC.
static const uint16_t MAX_DATA = 256 - 12;     // a page less the header and CRC

/// @brief Four characters as a magic number ("CHCR" -> 0x52434843).
/// @param s Exactly four characters, e.g. `save::magic("CHXX")`. Each game
///          needs its own: the pages are shared by everything ever uploaded.
/// @return The magic number (computed at compile time).
constexpr uint32_t magic(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 |
           (uint32_t)(uint8_t)s[2] << 16 | (uint32_t)(uint8_t)s[3] << 24;
}

/// @brief Whether saving works in this image.
/// @return false if the image is too big (it reaches into the save pages), or
///         a write did not verify.
bool available();                   // false: the image is too big, or a write failed

/// @brief Find the newest valid record.
/// @param magic   The game's magic number.
/// @param version The record's version: records of another version are ignored.
/// @param size    The data's size in bytes: records of another size are ignored.
/// @param flag    Optional: gets the header's flag byte.
/// @return A pointer to its data, in flash (read-only), or nullptr if there is
///         no record with this magic, version and size.
const void *read(uint32_t magic, uint8_t version, uint16_t size, uint8_t *flag = nullptr, uint8_t slot = 0);
/// @brief The page buffer to fill before write(): writing in place.
/// @return MAX_DATA bytes, zeroed, in the chunk scratch (so only between
///         gfx_wait() and the next flush).
void *buffer();
/// @brief Write what is in buffer() as a new record, to the older page.
/// @param magic   The game's magic number.
/// @param version The record's version.
/// @param size    How many bytes of buffer() to keep (at most MAX_DATA).
/// @param flag    A byte for the header (read back by read()'s flag).
/// @return true if the page was written and verified; false if saving is not
///         available or the write failed (then available() turns false).
bool write(uint32_t magic, uint8_t version, uint16_t size, uint8_t flag = 0, uint8_t slot = 0);

/// @brief Why saving does or doesn't work.
enum Status : uint8_t {
    OK,        ///< saving works (or nothing has failed yet)
    TOO_BIG,   ///< the image reaches into the save pages
    BROKEN,    ///< a write did not verify
    BLOCKED    ///< both pages hold other games' records; nothing was erased
};

/// @brief The state of saving; BLOCKED lets a game say "save area full of other games".
Status status();

/// @brief Flash ownership. A page that holds another game's record (a magic
///        that is not this game's) is never erased: the save goes to the other
///        page, in place, or fails with BLOCKED. Pass true to erase anyway
///        (e.g. after the player confirms). Default false.
void allowOverwrite(bool on);

/// @brief Hook for more than one save: a place saves can go instead of flash.
///
/// Every function gets a slot number (0 .. slots-1). The flash backend has
/// one slot; an SD-file backend (preallocated file, two alternating sectors,
/// generation + CRC, as Mystic Balloon does) can offer several. Same rules as
/// the flash calls: buffer() is filled, then write() runs between gfx_wait()
/// and the next flush.
struct Backend {
    const void *(*read)(uint32_t magic, uint8_t version, uint16_t size, uint8_t *flag, uint8_t slot);
    void *(*buffer)();
    bool (*write)(uint32_t magic, uint8_t version, uint16_t size, uint8_t flag, uint8_t slot);
    bool (*available)();
    uint8_t slots;
};
/// @brief Route save calls to a backend (nullptr: back to flash).
void useBackend(const Backend *b);
/// @brief How many save slots the current backend has (1 for flash).
uint8_t slots();

/// @brief The raw save pages, for a save manager (not for games).
/// @param i Page 0 or 1 (0xF500, 0xF600).
/// @return The page's 256 bytes in flash, or nullptr if the image reaches into it.
const uint8_t *rawPage(uint8_t i);
/// @brief Erase and program a whole page from `buf` (256 bytes, in RAM), then verify.
///        All 0xFF erases it. Like write(): only between gfx_wait() and the next flush.
/// @return true if it verified; false if the page is not usable or the write failed.
bool rawWrite(uint8_t i, const uint8_t *buf);

/// @brief Load a struct from the newest valid record.
/// @param magic   The game's magic number.
/// @param version The record's version.
/// @param data    Gets a copy of the record; left as it was if there is none.
/// @param flag    Optional: gets the header's flag byte.
/// @param slot    Which save (0 unless a backend offers more).
/// @return true if a record was found.
template <class T> bool load(uint32_t magic, uint8_t version, T &data, uint8_t *flag = nullptr, uint8_t slot = 0) {
    static_assert(sizeof(T) <= MAX_DATA, "save data must fit one flash page");
    const T *p = (const T *)read(magic, version, sizeof(T), flag, slot);
    if (!p) return false;
    data = *p;
    return true;
}
/// @brief Save a struct as a new record. Call between gfx_wait() and the next flush.
/// @param magic   The game's magic number.
/// @param version The record's version.
/// @param data    What to save (at most MAX_DATA bytes; checked at compile time).
/// @param flag    A byte for the header.
/// @return true if it was written and verified.
template <class T> bool store(uint32_t magic, uint8_t version, const T &data, uint8_t flag = 0, uint8_t slot = 0) {
    static_assert(sizeof(T) <= MAX_DATA, "save data must fit one flash page");
    if (!available()) return false;
    *(T *)buffer() = data;
    return write(magic, version, sizeof(T), flag, slot);
}

}  // namespace save

/// @}
