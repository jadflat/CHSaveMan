"""A real game's round trip through the app's menus, in the simulator: STARLIFT 0.38's two flash pages (a 512-byte
dump from STARLIFT's own simulator, its save_file.cpp format: page A then page B) are put in flash, then
BACK UP (both pages) -> ERASE A -> ERASE B -> RESTORE (SAVES.BAK, both pages, YES); the restored pages are written
to --out for STARLIFT's simulator to boot on.

    python CHSaveMan/tools/tests/roundtrip_starlift.py --pages PAGES.bin --out RESTORED.bin [--shots DIR]

Only a test shim is added (written to tools/tests/out/rt_starlift/, built into its own sim.exe there): the
library's simulator pages are loaded from and dumped to $CHSIM_SAVEFILE, as a game's own simulator save file would.
The app's code is built unchanged. Reuses sim_save.py's helpers (as sim_round3.py does)."""
import argparse
import os
from pathlib import Path

_src = (Path(__file__).resolve().parent / "sim_save.py").read_text(encoding="utf-8")
exec(compile(_src[:_src.index("# ==== 1. pages")], "sim_save_helpers", "exec"))

from chdrivelib import Driver, SimTransport  # noqa: E402  (on the path after the helpers)
from chsim import build  # noqa: E402
from fbimage import to_image  # noqa: E402
from simsave import ident_of  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--pages", required=True, help="512 bytes: flash page A (0xF500) then page B (0xF600)")
ap.add_argument("--out", required=True, help="where the restored pages go (512 bytes)")
ap.add_argument("--shots", help="a folder for screenshots of each step")
args = ap.parse_args()
pages = Path(args.pages).read_bytes()
if len(pages) != 512:
    raise SystemExit("--pages must be 512 bytes, not %d" % len(pages))
shots = Path(args.shots) if args.shots else None
if shots:
    shots.mkdir(parents=True, exist_ok=True)

RT = OUT / "rt_starlift"
RT.mkdir(parents=True, exist_ok=True)
SHIM = RT / "save_file.cpp"
SHIM.write_text(r'''// test shim: the save pages from $CHSIM_SAVEFILE at start-up, back to it at exit
#include <CHGame.h>
#include <stdio.h>
#include <stdlib.h>
namespace {
void dump() {
    const char *path = getenv("CHSIM_SAVEFILE");
    FILE *f = path ? fopen(path, "wb") : nullptr;
    if (!f) return;
    static const uint8_t erased[256] = {};
    for (uint8_t i = 0; i < 2; i++) { const uint8_t *p = save::rawPage(i); fwrite(p ? p : erased, 1, 256, f); }
    fclose(f);
}
struct Load {
    Load() {
        const char *path = getenv("CHSIM_SAVEFILE");
        if (!path) return;
        uint8_t buf[512];
        FILE *f = fopen(path, "rb");
        if (f) { if (fread(buf, 1, 512, f) == 512) { save::rawWrite(0, buf); save::rawWrite(1, buf + 256); } fclose(f); }
        atexit(dump);
    }
} load;
}
''', newline="")
EXE = RT / "sim.exe"
flash = RT / "flash.bin"


def sketch_copy():
    """The app's sources, unchanged, in RT/CHSaveMan: its simulator builds there, so a test running at the same
    time in the real sketch folder (its tools/chsim/build) is not raced."""
    dst = RT / "CHSaveMan"
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir(parents=True)
    for p in ROOT.iterdir():
        if p.is_file() and p.suffix in (".ino", ".h", ".cpp", ".c"):
            shutil.copy2(p, dst / p.name)
    shutil.copytree(ROOT / "src", dst / "src")
    (dst / "tools" / "chsim").mkdir(parents=True)
    shutil.copy2(ROOT / "tools" / "chsim" / "chdrive.py", dst / "tools" / "chsim" / "chdrive.py")
    return dst


class RtSession(Session):
    def __init__(self, game):
        self.game, self.prefix, self.fails = sketch_copy(), "STATE", 0
        flags = os.environ.get("CHSIM_FLAGS", "")
        os.environ["CHSIM_FLAGS"] = (flags + " " + str(SHIM)).strip()
        try:
            exe = build(self.game, out=EXE)
        finally:
            os.environ["CHSIM_FLAGS"] = flags
        self.d = Driver(SimTransport(exe), ident_of(self.game))
        self.d.handshake()
        self.d.cmd("L1")


def snap(name):
    if shots:
        to_image(s.d.shot(), 3).save(shots / (name + ".png"))


def st_line_raw(prefix):
    s.prefix = prefix
    r = s.say(QCMD) or ""
    s.prefix = "STATE"
    return r


for f in (bak, Path(str(bak) + ".tmp"), Path(str(bak) + ".und"), Path(str(bak) + ".utm"), Path(str(bak) + ".bk1")):
    f.unlink(missing_ok=True)
flash.write_bytes(pages)
os.environ["CHSIM_SAVEFILE"] = str(flash)
os.environ["SAVM_BAK"] = str(bak)
os.environ["SAVM_CARD"] = str(card("card_rt_starlift"))
for k in ("SAVM_NOCARD", "SAVM_FAIL", "SAVM_OFFER"):
    os.environ.pop(k, None)
s = RtSession(ROOT)
s.frames(3)
p0 = state()
print("pages in:  A %s\n           B %s" % (st_line_raw("STATE page=A"), st_line_raw("STATE page=B")), flush=True)
check(p0["B"][:2] == (3, "SLFT") and p0["B"][3], "page B holds a valid STARLIFT record (SLFT #%d)" % p0["B"][2])
snap("s0_main")

# BACK UP, both pages
menu(2, 6)
snap("s1_backup")
check("BACKED UP" in screen() and bak.exists(), "BACK UP: " + screen())
s.tap("A")
b = bak.read_bytes()
check(len(b) == 528 and b[:8] == b"CHSVBK01", "SAVES.BAK: 528 bytes, CHSVBK01")
check(b[16:528] == pages, "SAVES.BAK's two pages are byte-identical to the game's flash (512 bytes)")

# ERASE A, then ERASE B (page menu: ERASE THIS PAGE, then YES)
for pg in (0, 1):
    goto(pg); s.tap("A"); s.tap("A"); s.frames(2)
    snap("s2_erase%s_ask" % "AB"[pg])
    s.tap("DOWN"); s.tap("A"); s.frames(6)
    snap("s2_erase%s_done" % "AB"[pg])
    check("PAGE ERASED" in screen(), "ERASE %s: %s" % ("AB"[pg], screen()))
    s.tap("A"); s.frames(2)
    for _ in range(3): s.tap("B")
    s.frames(2)
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1, "both pages EMPTY after the erases: %r %r" % (p["A"][:2], p["B"][:2]))
snap("s3_erased")

# RESTORE: SAVES.BAK, BOTH PAGES, YES
goto(3); s.tap("A"); both(); s.frames(3)
snap("s4_restore_ask")
print("restore question:", ask(), flush=True)
s.tap("DOWN"); s.tap("A"); s.frames(6)
snap("s5_restored")
check("RESTORED" in screen(), "RESTORE: " + screen())
s.tap("A"); s.frames(2)
snap("s6_main")
p1 = state()
check(p1["B"][:4] == p0["B"][:4] and p1["A"][:4] == p0["A"][:4], "both pages read as before: %r %r" % (p1["A"][:4], p1["B"][:4]))

fails += s.result()                      # stdin closed: the simulator exits, the shim writes the pages
s = None
got = flash.read_bytes()
same = got == pages
print(("ok   " if same else "FAIL ") + "the restored flash pages are byte-identical to the game's (512 bytes)", flush=True)
if not same:
    fails += 1
    diff = [i for i in range(512) if got[i] != pages[i]]
    print("     %d bytes differ, first at %d" % (len(diff), diff[0]), flush=True)
Path(args.out).write_bytes(got)
print("roundtrip_starlift: " + ("ALL OK" if not fails else f"{fails} FAILED"), flush=True)
raise SystemExit(1 if fails else 0)
