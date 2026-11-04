"""0.19 simulator checks: an undo from SAVES.UND keeps that file until its pages are in flash (power cut, then a
normal undo); a failed back up / restore with NO CARD or CARD ERROR shows it on the main screen's BAK line and reads
SAVES.BAK again on the way back; RESTORE FROM keeps page B's counter visible; GAMES LIST shows the games right after a
multi-frame scan; a scan's new names reach SAVENAME.TXT after its message.
    python CHSaveMan/tools/tests/sim_v019.py
Reuses sim_save.py's helpers by executing its text up to its first test group."""
from pathlib import Path

_src = (Path(__file__).resolve().parent / "sim_save.py").read_text(encoding="utf-8")
exec(compile(_src[:_src.index("# ==== 1. pages")], "sim_save_helpers", "exec"))

und = Path(str(bak) + ".und")
un2 = Path(str(bak) + ".un2")
extra = [Path(str(bak) + e) for e in (".und", ".utm", ".un2", ".u2t", ".bk1", ".tmp")]


def st_line(prefix):
    s.prefix = prefix
    r = s.say(QCMD) or ""
    s.prefix = "STATE"
    return r


def fresh(**kw):
    for f in extra:
        f.unlink(missing_ok=True)
    start(keep_bak=False, **kw)


def erase_a():
    goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)     # page A: ERASE, YES
    s.tap("A"); s.frames(2)


def undo_yes():
    menu(6, 4); s.tap("DOWN"); s.tap("A"); s.frames(6)


# ==== 1. undo from SAVES.UND with a power cut after its first page write ====
fresh()
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
erase_a()
check(state()["A"][0] == 1 and und.exists(), "erase: page A empty, SAVES.UND written")
u0 = und.read_bytes()
check(u0[16:20] == b"CHAA" and u0[16 + 256:16 + 260] == b"CHBB", "SAVES.UND holds both records before the erase")
s.say("U")                                           # the power goes after the next page write
undo_yes()
check(state()["A"][:3] == (3, "CHAA", 5), "the cut undo wrote page A (#5)")
check(und.exists() and und.read_bytes() == u0, "a power cut during an undo leaves SAVES.UND as it was")
check(un2.exists() and un2.read_bytes()[16:20] == b"\xff" * 4 and un2.read_bytes()[16 + 256:16 + 260] == b"CHBB",
      "the pages before the undo are in SAVES.UN2 (A empty, B CHBB)")

# ==== 2. a normal undo: SAVES.UND becomes the pages before it, once they are in flash ====
fresh()
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
erase_a()
undo_yes()
r = screen()
check("UNDONE" in r and state()["A"][:3] == (3, "CHAA", 5), "undo puts page A back: " + r)
u1 = und.read_bytes() if und.exists() else b""
check(u1[16:20] == b"\xff" * 4 and u1[16 + 256:16 + 260] == b"CHBB", "after the undo SAVES.UND holds the pages before it")
s.tap("A"); s.frames(2)
undo_yes()
check(state()["A"][0] == 1, "a second undo swaps back (page A empty again)")
s.tap("A"); s.frames(2)

# ==== 3. NO CARD / CARD ERROR in a back up and a restore reach the BAK line, which is read again after ====
fresh()
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6)                                           # a good backup first: the BAK line shows the pages
s.tap("A"); s.frames(2)
check("CHAA" in st_line("STATE bak="), "BAK line after a backup: " + st_line("STATE bak="))
for fault, word in (("nocard", "NO_CARD"), ("cardfail", "CARD_ERROR")):
    s.say("Z " + fault)
    menu(2, 6)
    check("BACKUP FAILED" in screen(), "%s: back up fails: %s" % (fault, screen()))
    check(word in st_line("STATE bak="), "%s: the BAK line says %s at once: %s" % (fault, word, st_line("STATE bak=")))
    s.tap("A"); s.frames(3)
    check(word in st_line("STATE bak="), "%s: still %s on the main screen (read again, still failing)" % (fault, word))
    menu(3, 6)
    check("RESTORE FAILED" in screen() and word.replace("_", " ") in screen(), "%s: restore fails: %s" % (fault, screen()))
    s.say("Z -")                                     # the card is back: leaving the message reads SAVES.BAK again
    s.tap("A"); s.frames(3)
    check("CHAA" in st_line("STATE bak="), "%s: then the BAK line shows the pages again: %s" % (fault, st_line("STATE bak=")))

# ==== 4. RESTORE FROM: page B's counter stays visible with long names and big counters ====
cl = card("card_v019_src", names_txt="QLNG=LONGNAME GAMEX\nQLN2=OTHERLONG NAME\n", games=False)
fresh(card=cl)
say("W 0 QLNG 1 12345 40", "W 1 QLN2 1 23456 40")
menu(2, 6)
s.tap("A"); s.frames(2)
menu(3, 6)                                           # the RESTORE FROM list, cursor on SAVES.BAK
ln = st_line("STATE srcline=")[len("STATE srcline="):].rstrip("|")
check(len(ln.rstrip()) <= 27 and "#12345" in ln and "#23456" in ln and ln.startswith("A ") and " B " in ln,
      "RESTORE FROM row keeps both counters in 27 characters: %r" % ln)
for _ in range(3): s.tap("B")

# ==== 5. GAMES LIST right after a multi-frame scan; the new names in SAVENAME.TXT ====
cg = card("card_v019_list", games=False)
want = [("ALPHA.CHG", "Alpha Game", "QALP"), ("BETA.CHG", "Beta Game", "QBET"), ("SUB/GAMMA.CHG", "Gamma Game", "QGAM"),
        ("DELTA.CHG", "Delta Game", "QDEL")]
for p, title, code in want:
    chg(cg / "GAMES" / p, title, [code], pad=60000)
fresh(card=cg)
goto(4); s.tap("A")
n = 0
while screen().startswith("STATE screen=5") and n < 3000:
    s.frames(10); n += 10
r = screen()
check("SCAN DONE" in r and "4 GAMES  4 NEW" in r, "the scan: 4 games, 4 new: " + r)
s.frames(3)
t = (cg / "SAVENAME.TXT").read_text() if (cg / "SAVENAME.TXT").exists() else ""
check(all(("%s=%s\n" % (c, ti.upper())) in t for _, ti, c in want), "the new names are in SAVENAME.TXT: %r" % t)
s.tap("A"); s.frames(2)
menu(5, 6)
ls = dict(x.split("=", 1) for x in st_line("STATE list ").split()[2:])
check(screen().startswith("STATE screen=6") and ls.get("n") == "4", "GAMES LIST after the scan: 4 rows: %r %s" % (ls, screen()))
check(ls.get("row0", "").startswith("QALP"), "GAMES LIST's first row is ALPHA's code: %r" % ls.get("row0"))
for _ in range(2): s.tap("B")

fails += s.result()
print("ALL OK" if not fails else "%d FAILED" % fails)
raise SystemExit(1 if fails else 0)
