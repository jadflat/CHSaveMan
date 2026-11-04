"""Build a flat provisioning image: bootloader + application + metadata.

wchisp writes from address 0 and erases the whole code flash, so factory
provisioning and recovery need a single blob with everything in its final
place. The metadata page is written here exactly as the bootloader's own END
command writes it, so a provisioned device is indistinguishable from one that
was flashed over USB.
"""
from __future__ import annotations

import struct
import zlib

from . import layout as L
from .chg import TARGET_REV0


def check_boot_image(boot: bytes) -> None:
    """Refuse anything that is not a bootloader for this board: too small, over
    the reservation, or linked for another address (word 1 of the vector table
    is the reset address's high word on this core: 0 for the bootloader,
    APP_START for a sketch). The messages are the Go tool's."""
    if len(boot) < 256:
        raise ValueError(f"{len(boot)} bytes is too small to be a bootloader")
    if len(boot) > L.BOOT_SIZE:
        raise ValueError(f"bootloader is {len(boot)} bytes, over the {L.BOOT_SIZE}-byte reservation")
    linked = struct.unpack_from("<I", boot, 4)[0]
    if linked == 0:
        return
    if linked == L.APP_START:
        raise ValueError(f"this image is a sketch (linked for 0x{linked:04X}), not a bootloader")
    raise ValueError(f"this image is not a CHGame bootloader (linked for 0x{linked:08X})")


BOARD_WORD_OFFSET = 0x14        # a reserved vector slot: the board a bootloader image is for


def boot_image_board(boot: bytes) -> int:
    """The board a bootloader image is for: the target id at offset 0x14 of a
    board after rev0's, and rev0's (CX35) where that word is 0, as on every
    rev0 bootloader (platform/board/docs/protocol.md)."""
    word = struct.unpack_from("<I", boot, BOARD_WORD_OFFSET)[0] if len(boot) >= BOARD_WORD_OFFSET + 4 else 0
    return word or TARGET_REV0


def build_metadata(app: bytes, app_version: int = 0) -> bytes:
    meta = struct.pack(
        "<8I",
        L.META_MAGIC, L.META_VERSION, len(app), zlib.crc32(app) & 0xFFFFFFFF,
        app_version, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    )
    return meta + bytes([L.ERASED]) * (L.PAGE_SIZE - len(meta))


def pad_to_word(data: bytes) -> bytes:
    return data + bytes([L.ERASED]) * (-len(data) % 4)


def build_image(boot: bytes, app: bytes | None = None, app_version: int = 0) -> bytes:
    """Compose the full 62 KB flash image.

    With no application, the device comes up in the bootloader and waits, which
    is the correct state for a freshly provisioned board: it enumerates as a CDC
    port immediately and the first sketch can be uploaded over USB with no
    further use of the BOOT button.
    """
    if len(boot) > L.BOOT_SIZE:
        raise ValueError(
            f"bootloader is {len(boot)} bytes, over the {L.BOOT_SIZE}-byte reservation")

    image = bytearray([L.ERASED]) * L.FLASH_SIZE
    image[0:len(boot)] = boot

    if app is not None:
        app = pad_to_word(app)
        if len(app) > L.APP_MAX_SIZE:
            raise ValueError(
                f"application is {len(app)} bytes, over the {L.APP_MAX_SIZE}-byte region")
        image[L.APP_START:L.APP_START + len(app)] = app
        meta = build_metadata(app, app_version)
        image[L.META_ADDR:L.META_ADDR + len(meta)] = meta

    return bytes(image)


def trim(image: bytes) -> bytes:
    """Drop trailing erased bytes; there is no need to write 0xFF."""
    end = len(image)
    while end > 0 and image[end - 1] == L.ERASED:
        end -= 1
    return image[:end]
