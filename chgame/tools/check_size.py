"""Flash and RAM report for a CHGame build, from the linker map.

    chgame size [build/release] [--top 30] [--flash-limit N] [--ram-limit N] [--save-pages N]

(or `python tools/check_size.py BUILDDIR ...`). tools/device.py runs it after
every build.

The ceiling for statics + heap is 18,416 B (the 2 KB stack is fixed at the
top of SRAM); this reads _ebss from the map. The image is everything flash
holds: text, rodata and the .data initialisers (including RAM functions),
and it decides how many save pages are left. Exits non-zero when a limit is
exceeded, or fewer than --save-pages are left (a release build passes its
game's SAVE_PAGES, tools/gamecfg.py: two unless the game says otherwise).

With LTO (opt=oslto, the release build) the per-file table shows the link's
partitions (*.ltrans.o), not source files: use --symbols, or build with
opt=osstd for a per-file view.
"""
import argparse
import collections
import re
import sys
from pathlib import Path

RAM_LIMIT = 18416
FLASH_LIMIT = 50944


def parse(map_path):
    txt = Path(map_path).read_text(encoding="latin-1")
    sec = txt[txt.find("Linker script and memory map"):]
    per_file = collections.Counter()
    per_sym = collections.Counter()
    cur = None
    for line in sec.split("\n"):
        m = re.match(r"^ (\.\S+)\s*$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.*)$", line)
        if m:
            name, size, f = m.group(1), int(m.group(3), 16), m.group(4)
        else:
            m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*\.o.*)$", line)
            if not (m and cur):
                continue
            name, size, f = cur, int(m.group(2), 16), m.group(3)
            cur = None
        # .gnu.linkonce.r.*: RAM functions (loaded from flash, run from SRAM)
        if not re.match(r"\.(text|rodata|srodata|data|sdata|bss|sbss|gnu\.linkonce\.r)", name):
            continue
        kind = "ram" if re.match(r"\.(bss|sbss)", name) else "flash"
        fname = re.split(r"[\\/]", f.strip())[-1]
        if kind == "flash":
            per_file[fname] += size
            per_sym[name] += size
    sym = {}
    for key in ("_etext", "_data_lma", "_data_vma", "_edata", "_ebss", "_sbss"):
        m = re.search(r"0x([0-9a-f]+)\s+(?:PROVIDE \()?" + key + r"\b", txt)
        if m:
            sym[key] = int(m.group(1), 16)
    return per_file, per_sym, sym


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("build", nargs="?", default="build/release")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--flash-limit", type=int, default=FLASH_LIMIT)
    ap.add_argument("--ram-limit", type=int, default=RAM_LIMIT)
    ap.add_argument("--symbols", action="store_true")
    ap.add_argument("--save-pages", type=int, default=0, help="fail when fewer save pages are left")
    a = ap.parse_args(argv)
    maps = list(Path(a.build).glob("*.map"))
    if not maps:
        raise SystemExit(f"no .map in {a.build}")
    per_file, per_sym, sym = parse(maps[0])
    total = sum(per_file.values())
    print(f"flash (sections): {total} B")
    for f, s in per_file.most_common(a.top):
        print(f"  {s:6d}  {f}")
    if a.symbols:
        print("largest sections:")
        for n, s in per_sym.most_common(a.top):
            print(f"  {s:6d}  {n}")
    ok = True
    if "_data_lma" in sym and "_edata" in sym and "_data_vma" in sym:
        image = sym["_data_lma"] + (sym["_edata"] - sym["_data_vma"]) - 0x3000
        pages = 2 if image <= 0xF500 - 0x3000 else (1 if image <= 0xF600 - 0x3000 else 0)
        print(f"image: {image} B of {a.flash_limit} (save pages free: {pages}; two need <= {0xF500 - 0x3000})")
        ok &= image <= a.flash_limit
        if pages < a.save_pages:
            top = 0xF500 - 0x3000 if a.save_pages >= 2 else 0xF600 - 0x3000
            print(f"FAIL: {a.save_pages} save page{'s' if a.save_pages > 1 else ''} needed: "
                  f"the image is {image - top} B over {top} B")
            ok = False
    if "_ebss" in sym:
        ram = sym["_ebss"] - 0x20000010
        print(f"static RAM: {ram} B of {a.ram_limit} (stack 2048 B separate)")
        ok &= ram <= a.ram_limit
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
