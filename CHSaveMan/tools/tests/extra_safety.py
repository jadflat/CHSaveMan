"""Independent tester checks (never edits sim_save.py): save safety, games list, scan edges, error screens, budgets.
    python CHSaveMan/tools/tests/extra_safety.py
Reuses sim_save.py's helpers by executing its text up to its first test group."""
import re
import shutil
import subprocess
from pathlib import Path

_src = (Path(__file__).resolve().parent / "sim_save.py").read_text(encoding="utf-8")
exec(compile(_src[:_src.index("# ==== 1. pages")], "sim_save_helpers", "exec"))


def snapshot(*dirs):
    out = {}
    for d in dirs:
        for p in Path(d).rglob("*"):
            if p.is_file() and "GAMES" not in p.parts and "build" not in p.parts and "out" not in p.parts[-3:-1] or (p.is_file() and p.parent == OUT):
                out[str(p)] = p.read_bytes()
    return out


def list_state():
    s.prefix = "STATE list "
    r = s.say(QCMD)
    s.prefix = "STATE"
    return dict(x.split("=", 1) for x in r.split()[2:])


def newfiles(before, after, needles):
    return [k for k in after if before.get(k) != after[k] and not k.endswith("test.bak")
            and all(n in after[k] for n in needles)]


# ==== A. save safety ====
start(keep_bak=False)
say("E 0", "E 1")
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2); s.tap("A")
good = bak.read_bytes()

start(fail="crc")
say("W 0 CHAA 1 9 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(bak.read_bytes() == good, "A1 failed replace keeps the old SAVES.BAK byte-identical")
s.tap("A")

start(fail="io")
say("E 0", "E 1")
goto(3); s.tap("A"); both(); s.frames(4)
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1, "A2 restore under io fault writes nothing")
s.tap("A")

# A3 undo before restore (improvement 1)
start()
Path(str(bak) + ".und").unlink(missing_ok=True)    # 0.13: an undo file left by an earlier run would hide this one
say("W 0 CHZZ 1 20 40", "W 1 CHYY 1 21 40")
before = snapshot(OUT, ROOT)
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(8)
after = snapshot(OUT, ROOT)
p = state()
check(p["A"][1] == "CHAA" and p["B"][1] == "CHBB", "A3 restore itself works")
und = newfiles(before, after, [b"CHZZ", b"CHYY"])
print("   undo candidates:", und)
UNDO = bool(und)
check(UNDO, "A3 UNDO: restore wrote an undo file holding the overwritten pages")
s.tap("A")

# A4 undo before erase
start()
say("W 0 CHZZ 1 20 40")
before = snapshot(OUT, ROOT)
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(8)
after = snapshot(OUT, ROOT)
check(state()["A"][0] == 1, "A4 erase worked")
check(bool(newfiles(before, after, [b"CHZZ"])), "A4 UNDO: erase wrote an undo file holding the erased page")
s.tap("A")

# A5 undo write failure must stop the destructive op
if UNDO:
    start(fail="crc")
    say("W 0 CHZZ 1 20 40", "W 1 CHYY 1 21 40")
    goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(8)
    p = state()
    check(p["A"][1] == "CHZZ" and p["B"][1] == "CHYY", "A5 undo write fails: pages NOT overwritten")
    s.tap("A")

# A6 previous backup kept (improvement 2)
start(keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2); s.tap("A")
old = bak.read_bytes()
say("W 0 CHAA 1 7 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(6)
check(bak.read_bytes() != old, "A6 backup replaced")
kept = [str(p) for p in OUT.rglob("*") if p.is_file() and p != bak and p.read_bytes() == old]
check(bool(kept), "A6 PREV: the previous backup is kept as another file")
s.tap("A")

# A7 power cut: only .tmp
start(keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2); s.tap("A")
tmpf = Path(str(bak) + ".tmp"); good = bak.read_bytes(); bak.replace(tmpf)
say("E 0", "E 1")
goto(3); s.tap("A"); both(); s.frames(4)
check(ask() != [], "A7 restore from a lone .tmp asks")
s.tap("DOWN"); s.tap("A"); s.frames(6)
check(state()["A"][1] == "CHAA", "A7 restore from a lone .tmp works")
s.tap("A")
# A8 truncated .tmp beside a good bak
bak.write_bytes(good); tmpf.write_bytes(good[:300])
say("E 0", "E 1")
goto(3); s.tap("A"); both(); s.frames(4)
check(ask() != [], "A8 truncated .tmp beside a good bak: restore asks")
s.tap("B"); s.frames(2)
# A9 truncated bak only
tmpf.unlink(missing_ok=True)
bak.write_bytes(good[:300])
goto(3); s.tap("A"); both(); s.frames(4)
check("BAD BACKUP" in screen(), "A9 truncated SAVES.BAK: BAD BACKUP: " + screen())
s.tap("A")
say("E 0", "E 1")
goto(1); s.tap("A"); s.frames(2)
check("NOTHING HERE" in screen(), "A10 empty page B: NOTHING HERE")
s.tap("A")
# A11 write failure during restore
start(keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2); s.tap("A")
say("W 0 CHZZ 1 20 40", "W 1 CHYY 1 21 40")
s.say("F")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(8)
check("FAILED" in state()["screen"], "A11 failed write reported: " + state()["screen"])
s.tap("A")

# ==== B. games list scrolling ====
def mk(n, nosave=0):
    d = OUT / ("x_list%d_%d" % (n, nosave))
    shutil.rmtree(d, ignore_errors=True)
    for i in range(n):
        chg(d / "GAMES" / ("G%03d.CHG" % i), "Game %d" % i, ["GG%02d" % (i % 100)])
    for i in range(nosave):
        chg(d / "GAMES" / ("N%03d.CHG" % i), "No Save %d" % i, [])
    d.mkdir(parents=True, exist_ok=True)
    return d


for n, ns in ((1, 0), (11, 0), (12, 0), (40, 7)):
    d = mk(n, ns)
    tot = n + ns
    start(card=d)
    scan_card(400); s.tap("A")
    menu(5, 4)
    L = list_state()
    check(screen().startswith("STATE screen=6") and L.get("n") == str(tot), "B%d list shows %d games: %r" % (tot, tot, L))
    for _ in range(tot + 5): s.tap("DOWN")
    s.frames(3)
    top = int(list_state()["top"])
    check(0 <= top <= max(0, tot - 1), "B%d DOWN past the end stays in range (top=%d)" % (tot, top))
    for _ in range(tot + 5): s.tap("UP")
    s.frames(3)
    check(list_state()["top"] == "0", "B%d UP past the top stays at 0" % tot)
    for _ in range(8): s.tap("RIGHT")
    s.frames(3)
    top = int(list_state()["top"])
    check(top == max(0, tot - 11), "B%d RIGHT pages end at n-11=%d (got %d)" % (tot, max(0, tot - 11), top))
    for _ in range(8): s.tap("LEFT")
    s.frames(3)
    check(list_state()["top"] == "0", "B%d LEFT pages back to 0" % tot)
    s.tap("A"); s.frames(3)           # filter key (improvement 7): must not break the screen
    print("   after A:", screen()[:30], list_state())
    check(screen().startswith(("STATE screen=6", "STATE screen=0", "STATE screen=8")),
          "B%d A in the list keeps a sane screen (0.13: A opens the details, screen 8)" % tot)
    s.tap("B"); s.tap("B"); s.frames(2)

# ==== C. scan edge cases ====
c = OUT / "x_scan"
shutil.rmtree(c, ignore_errors=True)
chg(c / "GAMES" / "L1.CHG", "A" * 60, ["LNG1"])
chg(c / "GAMES" / "L2.CHG", b"Lower case t\xc3\xa9st", ["LNG2"])
chg(c / "GAMES" / "L3.CHG", "T" * 31, ["LNG3"])
chg(c / "GAMES" / "A" / "B" / "N.CHG", "Nested Two", ["NES2"])
chg(c / "GAMES" / "A" / "B" / "C" / "N3.CHG", "Nested Three", ["NES3"])
chg(c / "GAMES" / "U1.CHG", "Unsure", ["UNS1", "UNS2"])
chg(c / "GAMES" / "EMPTYT.CHG", b"", ["EMT1"])
chg(c / "GAMES" / "SP ACE.CHG", "Space Name", ["SPC1"])
(c / "GAMES" / "README.TXT").write_text("not a game")
(c / "GAMES" / "EMPTYDIR").mkdir()
start(card=c)
r = scan_card(200)
check("SCAN DONE" in r, "C1 scan edge card completes: " + r)
t = (c / "SAVENAME.TXT").read_text() if (c / "SAVENAME.TXT").exists() else ""
print(repr(t))
check("NES2=" in t and "NES3" not in t, "C2 nested level 2 scanned, level 3 not")
check("UNS1" not in t and "UNS2" not in t, "C3 unsure not learned")
check(all(len(l) < 80 and l.isascii() for l in t.splitlines()), "C4 learned lines short and ASCII")
check("LNG1=" in t and "LNG3=" in t and "SPC1=" in t, "C5 long/space titles learned")
gl = (c / "GAMELIST.TXT").read_text() if (c / "GAMELIST.TXT").exists() else ""
print(repr(gl))
gl_games = [x for x in gl.splitlines() if x and not x.startswith("#")]      # 0.13: a "# SKIPPED" line may end it
check("README" not in gl and len(gl_games) == 7, "C6 GAMELIST has only .CHG games (7): %d" % len(gl_games))
s.tap("A")
menu(5, 4)
check(list_state().get("n") == "7", "C7 list shows 7: %r" % list_state())
s.tap("B"); s.frames(2)
check("UNS1" in gl and "UNS2" in gl, "C8 UNSURE candidates written to GAMELIST (improvement 12)")

# ==== D. no card / card error screens ====
for mode in ("1", "fat"):
    start(nocard=mode)
    want = "NO CARD" if mode == "1" else "NOT FAT"
    menu(5, 4)
    check(want in screen() or "NO GAMELIST" in screen(), "D games list with nocard=%s: %s" % (mode, screen()))
    s.tap("A"); s.tap("B"); s.frames(2)
    check(want in scan_card(), "D scan with nocard=%s" % mode)
    s.tap("A"); s.frames(2)
    check(screen().startswith("STATE screen=0"), "D back at menu after error (%s)" % mode)
for f in ("io", "read", "scan"):
    start(card=c, fail=f)
    menu(5, 4)
    s.tap("A"); s.tap("B"); s.frames(2)
    check(screen().startswith("STATE screen=0") or screen().startswith("STATE screen=6"), "D games list survives fault %s: %s" % (f, screen()))

# ==== E. budgets (rebuilds with ./cl build) ====
o = subprocess.run(["bash", str(ROOT.parent / "cl"), "build"], cwd=ROOT.parent, capture_output=True, text=True).stdout
m = re.search(r"image: (\d+) B", o); r_ = re.search(r"static RAM: (\d+) B", o)
check(bool(m) and int(m.group(1)) <= 50432, "E flash image %s <= 50432" % (m and m.group(1)))
check(bool(r_) and int(r_.group(1)) <= 18416, "E RAM %s <= 18416" % (r_ and r_.group(1)))
check("__adddf3" not in o, "E no soft float")

fails += s.result()
print("ALL OK" if not fails else f"{fails} FAILED")
raise SystemExit(1 if fails else 0)
