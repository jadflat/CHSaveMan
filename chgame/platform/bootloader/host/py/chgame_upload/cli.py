"""chgame-upload: the CHGame bootloader's uploader (the Python one).

    chgame-upload [flags] probe                 list the boards and what each is running
    chgame-upload [flags] info                  the bootloader's version and memory map
    chgame-upload [flags] touch                 1200-baud touch: reboot a sketch into the bootloader
    chgame-upload [flags] run                   tell the bootloader to launch the application
    chgame-upload [flags] flash <file.bin> [-verify] [-run] [-device rev0]
    chgame-upload [flags] selfupdate <boot.bin> [--yes]
    chgame-upload [flags] provision -bootloader <boot.bin> [-app <app.bin>] [-wchisp <path>]
    chgame-upload [flags] burn -method usb|isp -bootloader <boot.bin> [-app <app.bin>] [-wchisp <path>]
    chgame-upload [flags] pack <file.bin> [-out <file.chg>] [-title T] [-author A] [-gameversion V] [-device rev0]
    chgame-upload [flags] noop

  flash       upload a sketch through the bootloader (what Upload does)
  selfupdate  replace the bootloader over USB, through the one installed
  provision   write the bootloader through the chip's factory ISP (hold BOOT, power cycle)
  burn        what Arduino's Burn Bootloader and Upload Using Programmer run:
              selfupdate (usb) or provision (isp), then the sketch if one is given
  pack        wrap a sketch image in a .chg package for the SD game menu (every
              build runs it, so Export Compiled Binary leaves one by the sketch)

-device names the board the image was built for (rev0; spec/chgame.md's
device table): pack writes its target id, and flash refuses a board whose
bootloader reports another. selfupdate and burn -method usb refuse a
bootloader image built for another board than the one running.

Flags: -port PORT, -timeout 10 (seconds; 500ms, 1m), -debug, -verbose, -quiet.
They may appear before or after the verb, and with one dash or two: the
board package's platform.txt spells them the Go way (-port, -run), and the
same recipe must run against this tool.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
import time

from . import __version__
from . import chg
from .client import (Client, NoDevice, SeveralDevices, ensure_bootloader, find_ports, quick_hello,
                     resolve_port, upload_touch, wait_for_application, wait_for_bootloader)
from .image import boot_image_board, check_boot_image
from .protocol import MODE_NAMES, StatusError
from . import layout as L
from . import upload as up
from .provision import WchispNotFound, provision

_GO_FLAG = re.compile(r"^-([a-z][a-z-]*)$")
_DURATION = re.compile(r"^(\d+(?:\.\d+)?)(ms|s|m|h)?$")


def normalise(argv):
    """`-port` and friends (the Go tool's spelling) as `--port`. A lone `-`,
    `--x` and negative numbers are left alone."""
    return ["--" + m.group(1) if (m := _GO_FLAG.match(a)) else a for a in argv]


def duration(s: str) -> float:
    """Seconds from `10`, `2.5`, `10s`, `500ms`, `1m` (Go's -timeout spellings)."""
    m = _DURATION.match(s.strip())
    if not m:
        raise argparse.ArgumentTypeError(f"not a duration: {s!r} (10, 2.5, 10s, 500ms, 1m)")
    v, unit = float(m.group(1)), m.group(2) or "s"
    return v * {"ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0}[unit]


def _common(top: bool) -> argparse.ArgumentParser:
    """The flags every verb takes, on both sides of the verb. The verb's copy
    has no defaults (SUPPRESS), or argparse would overwrite a value given
    before the verb with the default."""
    d = (lambda v: v) if top else (lambda v: argparse.SUPPRESS)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", default=d(None), help="serial port (auto-detected if omitted)")
    common.add_argument("--timeout", type=duration, default=d(10.0), help="per-command timeout (default 10 s)")
    common.add_argument("--debug", action="store_true", default=d(False), help="dump frames in both directions")
    # Arduino's platform.txt expands {upload.verbose} to one of these; accept
    # both so the same recipe works at either verbosity.
    common.add_argument("--verbose", action="store_true", default=d(False), help="more detail")
    common.add_argument("--quiet", action="store_true", default=d(False), help="less detail")
    return common


def parse_args(argv=None):
    common = _common(top=False)
    ap = argparse.ArgumentParser(prog="chgame-upload", description=__doc__, parents=[_common(top=True)],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"chgame-upload {__version__} (python)")
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="verb")
    sub.add_parser("probe", parents=[common], help="list CHGame ports and what mode each is in")
    sub.add_parser("info", parents=[common], help="full HELLO report from one device")
    sub.add_parser("touch", parents=[common], help="1200-baud touch: reboot a running sketch into the bootloader")
    sub.add_parser("run", parents=[common], help="tell the bootloader to launch the application")
    sub.add_parser("noop", parents=[common], help="no-op, satisfies Arduino's separate erase step")

    f = sub.add_parser("flash", parents=[common], help="upload an application image")
    f.add_argument("image")
    f.add_argument("--verify", action="store_true", help="read the region back and compare, independently of the device CRC")
    f.add_argument("--run", action="store_true", help="launch the application afterwards")
    f.add_argument("--device", help="the board the image is built for (rev0): refuse any other board")

    s = sub.add_parser("selfupdate", parents=[common], help="replace the bootloader itself, over USB")
    s.add_argument("image")
    s.add_argument("--yes", action="store_true", help="skip the confirmation prompt")

    k = sub.add_parser("pack", parents=[common], help="wrap a sketch image in a .chg package for the SD game menu")
    k.add_argument("image")
    k.add_argument("--out", help="default: the image's name with .chg for .bin")
    k.add_argument("--title", help="what the menu shows (default: the sketch's name in capitals)")
    k.add_argument("--author", default="")
    k.add_argument("--gameversion", default="", help="a short version string, e.g. 1.2")
    k.add_argument("--device", default="rev0", help="the board the image is built for (default rev0)")

    for name, help_ in (("provision", "first flash / recovery through the factory ISP (needs BOOT)"),
                        ("burn", "what Burn Bootloader runs: selfupdate (usb) or provision (isp), then the sketch")):
        p = sub.add_parser(name, parents=[common], help=help_)
        p.add_argument("--bootloader", required=True)
        p.add_argument("--app", help="write (burn: upload) a sketch too")
        p.add_argument("--wchisp", help="path to wchisp (else CHGAME_WCHISP or PATH)")
        if name == "burn":
            p.add_argument("--method", default="usb", help="usb (through the installed bootloader) or isp (factory ISP)")
    return ap.parse_args(normalise(list(sys.argv[1:] if argv is None else argv)))


# ---------------------------------------------------------------- the verbs

def cmd_probe(a) -> int:
    ports = find_ports()
    if not ports:
        print("no CHGame device found (16C0:27DD)")
        return 1
    for p in ports:
        try:
            h = quick_hello(p, 2.0, a.debug)        # a sketch never answers: a short, hard limit
            print(f"{p}: {MODE_NAMES.get(h.mode, h.mode)}, protocol v{h.proto_version}, bootloader v{h.boot_version}, "
                  f"board {chg.board_name(h.board)}")
        except Exception as e:
            print(f"{p}: application (did not answer HELLO: {type(e).__name__})")
    return 0


def cmd_info(a) -> int:
    port = resolve_port(a.port)
    with Client(port, timeout=a.timeout, debug=a.debug) as c:
        print(f"port          : {port}")
        print(c.hello().describe())
    return 0


def cmd_touch(a) -> int:
    port = resolve_port(a.port)
    upload_touch(port)
    boot = wait_for_bootloader(timeout=max(a.timeout, 10.0), port_hint=port)
    print(f"bootloader is up on {boot}" + ("" if boot == port else f" (was {port})"))
    return 0


def cmd_run(a) -> int:
    port = resolve_port(a.port)
    with Client(port, timeout=a.timeout, debug=a.debug) as c:
        h = c.hello()
        if h.mode != 1:
            raise SystemExit(f"{port} is in {MODE_NAMES.get(h.mode, h.mode)} mode, not the bootloader")
        if h.app_state != 0:
            raise SystemExit("bootloader reports the application image is not valid; refusing to RUN")
        c.run()
    print("sent RUN - the device should now be running the application")
    return 0


def cmd_noop(a) -> int:
    # Arduino runs a separate chip-erase step before Burn Bootloader. The erase
    # is done inside provision(), so that EVERY provisioning path gets it,
    # including Upload Using Programmer, which Arduino does not precede with
    # an erase step at all.
    print("erase: performed by the provisioning step itself")
    return 0


def _bar(done: int, total: int) -> None:
    width = 32
    filled = int(width * done / total) if total else width
    pct = 100 * done // total if total else 100
    print(f"\r  [{'#' * filled}{'.' * (width - filled)}] {pct:3d}%  {done}/{total} B", end="", flush=True)
    if done >= total:
        print()


def cmd_flash(a) -> int:
    r = up.flash_file(a.image, port=a.port, run=a.run, verify=a.verify, timeout=a.timeout,
                      progress=None if a.quiet else _bar, debug=a.debug, device=a.device)
    return 0 if r.get("readback_ok", True) else 1


def do_selfupdate(a, boot_path: str, app_path: str | None, interactive: bool) -> int:
    """Replace the bootloader over USB and, given a sketch, upload it afterwards
    (the update erases the one that was installed). Mirrors the Go tool's
    doSelfUpdate step for step."""
    boot_path = pathlib.Path(boot_path)
    boot = boot_path.read_bytes()
    try:
        check_boot_image(boot)
    except ValueError as e:
        raise SystemExit(f"{boot_path}: {e}")
    if app_path and not pathlib.Path(app_path).is_file():
        raise SystemExit(f"cannot read {app_path}")
    if interactive and not a.yes and sys.stdin.isatty():
        print("This replaces the BOOTLOADER itself.")
        print("  - the installed application is destroyed (it is the staging area)")
        print("  - if power is lost during the promotion, recovery is the BOOT")
        print("    button and the factory ISP (docs/recovery.md)")
        if input("proceed? [y/N] ").strip().lower() not in ("y", "yes"):
            return 1
    port = resolve_port(a.port)
    timeout = max(a.timeout, 10.0)
    try:
        boot_port = ensure_bootloader(port, timeout=timeout)
    except Exception as e:
        raise SystemExit(f"could not reach the bootloader on {port}: {e}\n"
                         'If the board has no working bootloader, use the programmer "WCH factory ISP" instead.')
    with Client(boot_port, timeout=timeout, debug=a.debug) as c:
        h = c.hello()
        print(f"port        : {boot_port}")
        print(f"installed   : bootloader v{h.boot_version}")
        print(f"bootloader  : {boot_path.name}  ({len(boot)} bytes)")
        if h.app_start != L.APP_START:
            raise SystemExit(f"this board reserves 0x{h.app_start:04X} bytes for its bootloader, not "
                             f"0x{L.APP_START:04X}: the image does not belong on it")
        # Another board's bootloader would drive the wrong pins: the panel and
        # the card stay dark and only the factory ISP brings the board back.
        if boot_image_board(boot) != h.board:
            raise SystemExit(f"this bootloader is built for {chg.board_name(boot_image_board(boot))}, but the "
                             f"board runs {chg.board_name(h.board)}'s: pick that board's bootloader")
        print("note        : the installed sketch is erased by the update. Keep the")
        print("              board powered until it is done (a few seconds).")
        up.selfupdate(c, boot, progress=None if a.quiet else _bar)
    print("promoting   : the board copies the new bootloader into place and resets")
    # The board is gone from USB while it copies and restarts.
    time.sleep(1.5)
    try:
        new_port = wait_for_bootloader(20.0, port_hint=boot_port)
    except TimeoutError as e:
        raise SystemExit(f"the board did not come back after the update: {e}\n"
                         "Unplug it and plug it in again. If it still does not appear, recover it\n"
                         'with the programmer "WCH factory ISP" (hold BOOT across a power cycle).')
    try:
        nh = quick_hello(new_port, 2.0, a.debug)
    except Exception as e:
        raise SystemExit(f"the board came back on {new_port} but did not identify itself: {e}")
    print(f"done        : bootloader v{nh.boot_version} is running on {new_port}")
    if not app_path:
        print("              no sketch is installed now: use Upload (or, with the SD")
        print("              menu bootloader, start a game from the card)")
        return 0
    up.flash_file(app_path, port=new_port, run=True, verify=False, timeout=a.timeout,
                  progress=None if a.quiet else _bar, debug=a.debug)
    return 0


def cmd_selfupdate(a) -> int:
    return do_selfupdate(a, a.image, None, interactive=True)


def cmd_provision(a) -> int:
    """Write the bootloader (and optionally a sketch) through the factory ISP."""
    try:
        provision(pathlib.Path(a.bootloader), pathlib.Path(a.app) if a.app else None, wchisp=a.wchisp)
    except (WchispNotFound, RuntimeError) as e:
        raise SystemExit(str(e))
    return 0


def cmd_pack(a) -> int:
    out = a.out or chg.default_output(a.image)
    title = a.title or chg.default_title(a.image)
    data = chg.pack(pathlib.Path(a.image).read_bytes(), title, a.author, a.gameversion, device=a.device)
    pathlib.Path(out).write_bytes(data)
    if not a.quiet:
        print(f"SD menu package: {out} ({title}, {len(data) - chg.HEADER_BYTES} B)")
    return 0


def cmd_burn(a) -> int:
    if a.method == "usb":
        a.yes = True
        return do_selfupdate(a, a.bootloader, a.app, interactive=False)
    if a.method == "isp":
        return cmd_provision(a)
    raise SystemExit(f'unknown method "{a.method}" (usb or isp)')


def main(argv=None) -> int:
    a = parse_args(argv)
    try:
        return {"probe": cmd_probe, "info": cmd_info, "touch": cmd_touch, "run": cmd_run, "noop": cmd_noop,
                "flash": cmd_flash, "selfupdate": cmd_selfupdate, "provision": cmd_provision,
                "burn": cmd_burn, "pack": cmd_pack}[a.cmd](a)
    except (NoDevice, SeveralDevices) as e:
        raise SystemExit(str(e))
    except StatusError as e:
        raise SystemExit(f"device refused: {e}")
    except TimeoutError as e:
        raise SystemExit(f"no response: {e}")
    except (ValueError, OSError) as e:
        raise SystemExit(str(e))


if __name__ == "__main__":
    raise SystemExit(main())
