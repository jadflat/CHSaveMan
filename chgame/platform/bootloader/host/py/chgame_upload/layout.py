"""CHGame flash layout — host-side mirror of bootloader/src/chgame_map.h.

The C header is the single source of truth. This mirror exists because the
uploader ships inside the Arduino platform, where that header is not present.

`test/protocol/test_layout.py` asserts the two agree, so a change to the header
that is not reflected here fails the test suite rather than producing an
uploader that silently writes to the wrong address.
"""
from __future__ import annotations

FLASH_BASE = 0x00000000
FLASH_SIZE = 0x0000F800        # 62 KB user code flash on the CH32X035G8U6
PAGE_SIZE = 256                # fast erase/program granularity

BOOT_START = 0x00000000
BOOT_SIZE = 0x00003000         # 12 KB bootloader reservation

APP_START = BOOT_START + BOOT_SIZE          # 0x3000
META_ADDR = FLASH_BASE + FLASH_SIZE - PAGE_SIZE   # 0xF700
APP_MAX_SIZE = META_ADDR - APP_START        # 50944
APP_END = META_ADDR

META_MAGIC = 0x4D474843        # "CHGM"
META_VERSION = 1
BOOT_MAGIC = 0x43484742        # "CHGB"

ERASED = 0xFF


def as_dict() -> dict[str, int]:
    return {
        "CHGAME_FLASH_BASE": FLASH_BASE,
        "CHGAME_FLASH_SIZE": FLASH_SIZE,
        "CHGAME_PAGE_SIZE": PAGE_SIZE,
        "CHGAME_BOOT_START": BOOT_START,
        "CHGAME_BOOT_SIZE": BOOT_SIZE,
        "CHGAME_APP_START": APP_START,
        "CHGAME_META_ADDR": META_ADDR,
        "CHGAME_APP_MAX_SIZE": APP_MAX_SIZE,
        "CHGAME_APP_END": APP_END,
        "CHGAME_META_MAGIC": META_MAGIC,
        "CHGAME_META_VERSION": META_VERSION,
        "CHGAME_BOOT_MAGIC": BOOT_MAGIC,
    }
