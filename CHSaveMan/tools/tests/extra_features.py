"""Tester checks for 0.13/0.14 features, aimed at data-loss paths. python CHSaveMan/tools/tests/extra_features.py"""
import shutil
from pathlib import Path

_src = (Path(__file__).resolve().parent / "sim_save.py").read_text(encoding="utf-8")
exec(compile(_src[:_src.index("# ==== 1. pages")], "sim_save_helpers", "exec"))
und = Path(str(bak) + ".und")
bk1 = Path(str(bak) + ".bk1")
tmpf = Path(str(bak) + ".tmp")


def st_line(prefix):
    s.prefix = prefix
    r = s.say(QCMD) or ""
    s.prefix = "STATE"
    return r


def pending():
    return int(dict(x.split("=", 1) for x in screen().split()[1:5])["pending"])


def list_state():
    r = st_line("STATE list ")
    return dict(x.split("=", 1) for x in r.split()[2:])


def det_state():
    return dict(x.split("=", 1) for x in st_line("STATE det ").split()[2:])


def clean():
    for p in (bak, und, bk1, tmpf):
        p.unlink(missing_ok=True)


def mkbak(a="CHAA 1 5", b="CHBB 1 6"):
    say("W 0 %s 40" % a, "W 1 %s 40" % b)
    menu(2, 6); s.tap("A")


# F1 bad SAVES.BAK + REPLACE YES must not destroy a good BK1
start(keep_bak=False); clean()
mkbak()
good1 = bak.read_bytes()
say("W 0 CHAA 1 6 40"); menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("A")   # bk1 = first
check(bk1.exists() and bk1.read_bytes() == good1, "F1 setup: BK1 holds the first backup")
raw = bytearray(bak.read_bytes()); raw[100] ^= 0xFF; bak.write_bytes(bytes(raw))        # corrupt SAVES.BAK
say("W 0 CHAA 1 7 40"); menu(2, 4)
print("   screen after backup with corrupt BAK:", screen())
if pending():
    s.tap("DOWN"); s.tap("A"); s.frames(5)
check(bk1.exists() and bk1.read_bytes() == good1, "F1 a corrupt SAVES.BAK replaced must not overwrite the good SAVES.BK1")
s.tap("A")

# F2 erase with the undo unwritable (crc fault): erase goes on, says NO UNDO FILE; the only save is gone
start(fail="crc"); clean()
say("W 0 CHZZ 1 20 40", "E 1")
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(8)
print("   F2 erase with undo failing:", state()["screen"], "| page A kind", state()["A"][0])
check("NO UNDO" in state()["screen"] or state()["A"][1] == "CHZZ", "F2 erase with no undo file is reported (or refused)")
s.tap("A")

# F3 restore with the undo unwritable stops and keeps the pages
start(keep_bak=False); clean()
mkbak()
start(fail="crc")
say("W 0 CHZZ 1 20 40", "W 1 CHYY 1 21 40")
goto(3); s.tap("A"); both(); s.frames(4); s.tap("DOWN"); s.tap("A"); s.frames(8)
p = state()
check(p["A"][1] == "CHZZ" and p["B"][1] == "CHYY", "F3 restore stops when SAVES.UND cannot be written")
s.tap("A")

# F4 undo twice swaps back; undo file stays valid; undo when SAVES.UND corrupt is refused
start(keep_bak=False); clean()
mkbak()
say("W 0 CHZZ 1 20 40", "W 1 CHYY 1 21 40")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(8); s.tap("A")
check(und.exists() and b"CHZZ" in und.read_bytes(), "F4 UND holds CHZZ/CHYY")
raw = bytearray(und.read_bytes()); raw[100] ^= 0xFF; und.write_bytes(bytes(raw))
goto(6); s.tap("A"); s.frames(5)
check(state()["A"][1] == "CHAA" and "BAD" in screen().upper() or state()["A"][1] == "CHAA", "F4 corrupt SAVES.UND does not change pages: " + screen())
s.tap("A")

# F5 .SAV with bad CRC: restore refused, pages unchanged; seq near wrap
start(card=card("f5", games=False), keep_bak=False); clean()
c5 = OUT / "f5"
say("W 0 CHAA 1 65535 40", "W 1 CHBB 1 3 40")
open_ok = False
chg(c5 / "GAMES" / "AA.CHG", "Game Aa", ["CHAA"])
scan_card(60); s.tap("A")
menu(5, 4); s.tap("A"); s.frames(4)
print("   F5 details:", det_state())
# details of a game in flash: BACK UP ITS SAVE (option 2 when no .SAV)
pass
s.tap("A"); s.frames(5)
sav = c5 / "SAVES" / "CHAA.SAV"
check(sav.exists() and len(sav.read_bytes()) == 272, "F5 BACK UP ITS SAVE wrote CHAA.SAV: " + screen())
s.tap("A") if pending() == 0 else None
s.frames(2)
if sav.exists():
    raw = bytearray(sav.read_bytes()); raw[100] ^= 0xFF; sav.write_bytes(bytes(raw))
    s.tap("B"); s.tap("B"); s.frames(2)
    say("W 0 CHAA 1 65535 40", "W 1 CHBB 1 3 40")
    goto(3); s.tap("A"); s.frames(5)
    print("   F5 restore list:", st_line("STATE src "))
    n = [x for x in st_line("STATE src ").split() if x.startswith("files=")]
    check(n and "CHAA.SAV:4" in n[0], "F5 a corrupt .SAV is listed BAD_FILE: %r" % n)

# F6 backup offer
start(keep_bak=False, card=OUT / "f6"); clean()
import os
start(keep_bak=False, card=OUT / "f6", offer=True)
say("W 0 CHAA 1 5 40"); s.say("O"); s.frames(4)
print("   F6 offer screen:", screen())
check(pending() != 0 or "BACK UP NOW" in screen().upper(), "F6 offer appears with an unsaved page")
if pending():
    s.tap("A"); s.frames(4)                     # default NO
    check(not bak.exists(), "F6 NO (default) writes no backup")
    s.say("O"); s.frames(4)
    s.tap("DOWN"); s.tap("A"); s.frames(5)
    check(bak.exists(), "F6 YES backs up")
    s.tap("A")
    s.say("O"); s.frames(4)
    check(pending() == 0, "F6 no offer once SAVES.BAK holds the newest save: " + screen())
# offer with a failing card must not hang the menu
start(fail="io", card=OUT / "f6", offer=True)
say("W 0 CHAA 1 5 40"); s.say("O"); s.frames(4)
check(screen().startswith("STATE screen="), "F6 offer under io fault does not crash: " + screen())

# F7 CSV with quotes / commas / newline-ish titles; IGNORE.TXT edge cases
c7 = OUT / "f7"
shutil.rmtree(c7, ignore_errors=True)
chg(c7 / "GAMES" / "Q.CHG", 'He said "hi", ok', ["QQQ1"])
chg(c7 / "GAMES" / "NO SAVE.CHG", "Plain", [])
chg(c7 / "GAMES" / "BIGGEST.CHG", "Z" * 30, ["QQQ2"])
start(card=c7, keep_bak=False); clean()
scan_card(60); s.tap("A")
menu(5, 4); s.tap("START"); s.frames(5)
t = (c7 / "GAMELIST.CSV").read_bytes().decode() if (c7 / "GAMELIST.CSV").exists() else ""
print("   CSV:", repr(t))
check('He said ""hi"", ok' in t or 'He said "hi"' not in t.replace('""', ''), "F7 CSV doubles quotes inside the title field")
check(t.count("\r\n") == 4 and "\n" not in t.replace("\r\n", ""), "F7 CSV: header + 3 rows, CRLF only")
s.tap("A")
# ignore twice, file without trailing newline, >1 KB IGNORE.TXT
ig = c7 / "IGNORE.TXT"
ig.write_text("OTHER.CHG")                      # no trailing newline
s.frames(2)
for _ in range(2):                              # ignore Q.CHG twice via details
    s.tap("B") if screen().startswith("STATE screen=6") else None
menu(5, 4); s.tap("A"); s.frames(4)
D = det_state()
s.tap("A") if D.get("path") else None
s.frames(4)
ign_text = ig.read_text()
print("   IGNORE.TXT:", repr(ign_text))
check("OTHER.CHG" in ign_text and ign_text.count("\n") <= 3 and "OTHER.CHGQ" not in ign_text.replace("\n", ""),
      "F7 appending to an IGNORE.TXT with no trailing newline keeps both lines separate")
s.tap("B"); s.tap("B"); s.frames(2)
ig.write_text("# pad\n" + "".join("X%03d.CHG\n" % i for i in range(150)))        # > 1 KB
print("   ignore size", ig.stat().st_size)
menu(5, 4); s.tap("A"); s.frames(4)
before = ig.read_bytes()
s.tap("A"); s.frames(4)
check(ig.read_bytes() == before or ig.stat().st_size >= len(before), "F7 IGNORE with a >1 KB IGNORE.TXT never truncates it: %d -> %d" % (len(before), ig.stat().st_size))
print("   screen:", screen())
s.tap("B"); s.tap("B"); s.frames(2)

# F8 RESTORE-from-card list with many .SAV files
c8 = OUT / "f8"
shutil.rmtree(c8, ignore_errors=True)
(c8 / "GAMES").mkdir(parents=True)
start(card=c8, keep_bak=False); clean()
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
sd = c8 / "SAVES"; sd.mkdir()
good = None
for i in range(12):
    say("W 0 GG%02d 1 %d 40" % (i, 5 + i));
    # write .SAV via the app (page menu: BACK UP THIS GAME, option after BACK UP...)
good = None
files_before = list(sd.glob("*"))
# craft .SAV files by hand from a backup's page A: header(16)+256
mkbak("CHAA 1 5", "CHBB 1 6")
b = bak.read_bytes()
import zlib
for i in range(12):
    page = bytearray(b[16:16 + 256]); page[0:4] = b"GG%02d" % i
    # copy header of a real .SAV is unknown, so only exercise the list with junk entries:
    (sd / ("JUNK%02d.SAV" % i)).write_bytes(bytes(40) if i % 2 else b"")
goto(3); s.tap("A"); s.frames(8)
src = st_line("STATE src ")
print("   F8 list:", src)
check(screen().startswith("STATE screen=9"), "F8 restore list with 12 junk .SAV files opens")
for _ in range(20): s.tap("DOWN")
s.frames(3)
for _ in range(25): s.tap("UP")
s.frames(3)
check(screen().startswith("STATE screen=9"), "F8 scrolling the list past both ends is stable")
s.tap("DOWN"); s.tap("DOWN"); s.tap("A"); s.frames(5)       # a junk entry
check(state()["A"][1] == "CHAA" and state()["B"][1] == "CHBB", "F8 choosing a junk .SAV changes no page: " + screen())
s.tap("A"); s.tap("B"); s.frames(2)

# F9 card removed mid-flow: restore list with no card / list fault
for mode in ("1", "fat"):
    start(nocard=mode)
    goto(3); s.tap("A"); both(); s.frames(5)
    check("NO CARD" in screen() or "NOT FAT" in screen(), "F9 restore list with nocard=%s: %s" % (mode, screen()))
    s.tap("A")
start(fail="io")
goto(3); s.tap("A"); s.frames(5)
check(screen().startswith("STATE screen=") , "F9 restore list under io fault: " + screen())
s.tap("A")

fails += s.result()
print("ALL OK" if not fails else f"{fails} FAILED")
raise SystemExit(1 if fails else 0)
