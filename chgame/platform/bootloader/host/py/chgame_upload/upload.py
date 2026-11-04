"""Firmware upload over the CHGame bootloader protocol."""
from __future__ import annotations

import pathlib
import struct
import time
import zlib
from typing import Callable

from .chg import board_name, target_of
from .client import Client, ensure_bootloader, resolve_port, wait_for_application
from .protocol import (
    CMD_ABORT, CMD_BEGIN, CMD_DEV_UNLOCK, CMD_DEV_WRITE_BOOT, CMD_END,
    CMD_READ, CMD_WRITE, ST_OK, MODE_NAMES, STATUS_NAMES, StatusError,
)

DEV_KEY = 0x43484744  # "CHGD": an interlock against accidents, not a secret.
ERASE_TIMEOUT = 30.0  # BEGIN erases up to ~200 pages; END recomputes the CRC from flash
PROMOTE_TIMEOUT = 10.0


def chunk_size(max_payload: int, page: int = 256) -> int:
    """Largest WRITE data chunk the device will accept.

    Prefer whole flash pages so the device commits one page per chunk, but never
    exceed what it advertised - the bootloader currently caps frames at one USB
    packet, which is well under a page.
    """
    usable = max_payload - 4          # 4-byte offset field
    if usable >= page:
        return (usable // page) * page
    return usable


def pad_to_word(image: bytes) -> bytes:
    """The bootloader requires a word-multiple length (CRC span and metadata)."""
    return image + b"\xff" * (-len(image) % 4)


def upload(c: Client, image: bytes,
           progress: Callable[[int, int], None] | None = None,
           verify_readback: bool = False) -> dict:
    """BEGIN / WRITE* / END. Returns timing and size info.

    Raises StatusError with the device's own status code on refusal, so callers
    can distinguish "image too large" from "flash failed" from "CRC mismatch".
    """
    h = c.hello()
    if h.mode != 1:
        raise RuntimeError(f"device is not in bootloader mode (mode={h.mode})")

    image = pad_to_word(image)
    if len(image) > h.app_max_size:
        raise ValueError(
            f"image is {len(image)} bytes; the application region holds {h.app_max_size}"
        )

    crc = zlib.crc32(image) & 0xFFFFFFFF
    step = chunk_size(h.max_payload, h.page_size)

    t0 = time.monotonic()
    c.check(CMD_BEGIN, struct.pack("<II", len(image), crc), timeout=ERASE_TIMEOUT)
    t_erase = time.monotonic() - t0

    sent = 0
    while sent < len(image):
        piece = image[sent:sent + step]
        c.check(CMD_WRITE, struct.pack("<I", sent) + piece)
        sent += len(piece)
        if progress:
            progress(sent, len(image))
    t_write = time.monotonic() - t0 - t_erase

    c.check(CMD_END, timeout=ERASE_TIMEOUT)
    t_total = time.monotonic() - t0

    result = {
        "bytes": len(image), "crc32": crc, "chunk": step,
        "erase_s": t_erase, "write_s": t_write, "total_s": t_total,
        "kbps": (len(image) / 1024.0) / t_write if t_write > 0 else 0.0,
    }

    if verify_readback:
        # Bootloader v2 (BOOT_VERSION 3) has no READ: END's own CRC check of
        # what is in flash is the check there (None: not available).
        result["readback_ok"] = (readback_matches(c, h.app_start, image, h.max_payload)
                                 if h.boot_version < 3 else None)

    return result


def readback_matches(c: Client, app_start: int, image: bytes,
                     max_payload: int = 56) -> bool:
    """Independent check: read the region back and compare byte for byte.

    END already verifies a CRC computed on the device. This verifies through a
    different path entirely, which is what catches a bootloader that computes
    its CRC over the wrong span or reports success it did not achieve.
    """
    step = max_payload - 1            # response payload is status byte + data
    off = 0
    while off < len(image):
        n = min(step, len(image) - off)
        r = c.check(CMD_READ, struct.pack("<IH", app_start + off, n))
        if r[1:1 + n] != image[off:off + n]:
            return False
        off += n
    return True


def abort(c: Client) -> None:
    c.check(CMD_ABORT)


def selfupdate(c: Client, boot_image: bytes,
               progress: Callable[[int, int], None] | None = None) -> dict:
    """Replace the bootloader itself.

    Stages the new bootloader through the ordinary upload path (so it gets the
    same bounds checks, per-page verify and CRC as any firmware), then promotes
    it with DEV_WRITE_BOOT.

    The device is briefly without a usable bootloader. If that is interrupted,
    recovery is the BOOT button and the factory ISP (docs/recovery.md).
    This also destroys the installed application, because the staging area IS
    the application region; re-upload afterwards.
    """
    boot_image = pad_to_word(boot_image)
    # Unlock BEFORE staging. Staging ends with valid metadata over the staged
    # copy; if the unlock were refused after that, a bootloader image linked
    # for address 0 would be left looking like a launchable application.
    # (BOOT_VERSION 2 and later also refuse to launch an image carrying the
    # bootloader signature, CHGAME_BOOT_SIG, but older ones do not.)
    try:
        c.check(CMD_DEV_UNLOCK, struct.pack("<I", DEV_KEY))
    except StatusError as e:
        if STATUS_NAMES.get(e.status) in ("ERR_LOCKED", "ERR_BADCMD"):
            raise RuntimeError("the installed bootloader is locked: it does not accept a bootloader update "
                               'over USB.\nUse the programmer "WCH factory ISP" instead') from e
        raise
    try:
        info = upload(c, boot_image, progress=progress)
    except Exception as e:
        raise RuntimeError(f"staging failed, the installed bootloader is untouched: {e}") from e

    crc = zlib.crc32(boot_image) & 0xFFFFFFFF
    try:
        r = c.request(CMD_DEV_WRITE_BOOT, struct.pack("<II", len(boot_image), crc), timeout=PROMOTE_TIMEOUT)
        if r and r[0] != ST_OK:
            raise RuntimeError(f"the device refused to replace its bootloader ({STATUS_NAMES.get(r[0], hex(r[0]))}); "
                               "the installed bootloader is untouched")
    except (TimeoutError, OSError):
        # The device resets as part of this command; a lost acknowledgement is
        # expected rather than a failure.
        pass

    info["promoted"] = True
    return info


def flash_file(image_path, *, port: str | None = None, run: bool = True, verify: bool = False,
               timeout: float = 10.0, progress: Callable[[int, int], None] | None = None,
               log: Callable[[str], None] | None = print, debug: bool = False,
               device: str | None = None) -> dict:
    """Upload an image file the way the `flash` verb does: find the board, put
    it in the bootloader (the 1200-baud touch), upload, optionally read back,
    optionally RUN and wait for the sketch to come back. Returns upload()'s
    dict plus "port" (the bootloader's) and, after RUN, "app_port". `device`,
    the board the image was built for ("rev0"), refuses a board whose
    bootloader says it is another (HELLO's board field); None checks nothing.
    Raises NoDevice / SeveralDevices / StatusError / TimeoutError / ValueError."""
    say = log or (lambda s: None)
    image_path = pathlib.Path(image_path)
    image = image_path.read_bytes()
    want = target_of(device) if device is not None else None
    port = resolve_port(port)
    timeout = max(timeout, 10.0)

    # Do the whole transition here rather than relying on the IDE. One actor
    # owning the port through app -> bootloader -> app is what makes this
    # reliable; two actors racing for it is the classic native-USB upload bug.
    boot_port = ensure_bootloader(port, timeout=timeout)
    if boot_port != port:
        say(f"note    : bootloader appeared on {boot_port} (was {port})")

    with Client(boot_port, timeout=timeout, debug=debug) as c:
        h = c.hello()
        if h.mode != 1:
            raise RuntimeError(f"{boot_port} is in {MODE_NAMES.get(h.mode, h.mode)} mode, not the bootloader")
        say(f"port    : {boot_port}")
        say(f"image   : {image_path}  ({len(image)} bytes)")
        say(f"region  : 0x{h.app_start:04X} + {h.app_max_size} bytes")
        if want is not None and h.board != want:
            raise ValueError(f"the image is built for {board_name(want)}, but this board's bootloader is "
                             f"{board_name(h.board)}'s: build for that board (Tools > Board)")
        if len(image) > h.app_max_size:
            raise ValueError(f"image does not fit: {len(image)} > {h.app_max_size}")
        r = upload(c, image, progress=progress, verify_readback=verify)
        r["port"] = boot_port
        say(f"crc32   : 0x{r['crc32']:08X}")
        say(f"erase   : {r['erase_s']:.2f} s")
        say(f"write   : {r['write_s']:.2f} s  ({r['kbps']:.1f} KiB/s, {r['chunk']} B chunks)")
        if verify:
            ok = r["readback_ok"]
            say("readback: " + ("MATCHES" if ok else "MISMATCH" if ok is False else
                                "not available (bootloader v3 has no READ; END checked the CRC in flash)"))
            if ok is False:
                return r
        say(f"total   : {r['total_s']:.2f} s  -- image accepted and marked valid")
        if run:
            c.run()
            say("sent RUN")
    if run:
        # Confirm the sketch actually came back, so a silent failure to launch
        # is reported here rather than discovered later by the user.
        try:
            r["app_port"] = wait_for_application(6.0)
            say(f"running : application is up on {r['app_port']}")
        except TimeoutError:
            say("warning : the application did not re-enumerate within 6s")
    return r
