"""Round-3 simulator checks (0.18): every screen with NO CARD / CARD NOT FAT32 / CARD ERROR (at boot and at run time),
a failed SAVENAME.TXT append written once later, a multi-frame scan that must name every game, power cuts in the
undo and .SAV writes, the restore and erase read-back, and a full round trip of a game's record.
    python CHSaveMan/tools/tests/sim_round3.py
Reuses sim_save.py's helpers by executing its text up to its first test group."""
import shutil
import struct
from pathlib import Path

_src = (Path(__file__).resolve().parent / "sim_save.py").read_text(encoding="utf-8")
exec(compile(_src[:_src.index("# ==== 1. pages")], "sim_save_helpers", "exec"))

und = Path(str(bak) + ".und")
utm = Path(str(bak) + ".utm")
bk1 = Path(str(bak) + ".bk1")
tmpf = Path(str(bak) + ".tmp")


def st_line(prefix):
    s.prefix = prefix
    r = s.say(QCMD) or ""
    s.prefix = "STATE"
    return r


def bak_line():
    r = st_line("STATE bak=")
    return r.split(" ")[1].split("=", 1)[1] if " " in r else r


def list_state():
    r = st_line("STATE list ")
    return dict(x.split("=", 1) for x in r.split()[2:])


def pending():
    return int(dict(x.split("=", 1) for x in screen().split()[1:5])["pending"])


def every_screen(word, label):
    """Each card job from the main menu reports `word`; nothing is written to flash."""
    before = (st_line("STATE page=A"), st_line("STATE page=B"))
    for n, what in ((2, "BACK UP"), (3, "RESTORE"), (4, "FIND GAME NAMES"), (5, "GAMES LIST"), (6, "UNDO")):
        goto(n); s.tap("A"); s.frames(6)
        if screen().startswith("STATE screen=7"):          # a page chooser first (back up): BOTH
            s.tap("A"); s.frames(6)
        check(word in screen(), "%s: %s says %s: %s" % (label, what, word, screen()))
        s.tap("A"); s.frames(2)
        check(screen().startswith("STATE screen=0"), "%s: back on the main menu after %s" % (label, what))
    goto(0); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)  # page A's menu: BACK UP THIS GAME
    check(word in screen(), "%s: BACK UP THIS GAME says %s: %s" % (label, word, screen()))
    s.tap("A"); s.frames(2)
    for _ in range(3): s.tap("B")
    s.frames(2)
    check((st_line("STATE page=A"), st_line("STATE page=B")) == before, "%s: the flash pages are untouched" % label)


# ==== 1. no card, no FAT volume, a card that fails init: at boot, then at run time ====
for nocard, word, bakword in (("1", "NO CARD", "NO_CARD"), ("fat", "CARD NOT FAT32", "CARD_NOT_FAT32"), ("err", "CARD ERROR", "CARD_ERROR")):
    start(nocard=nocard, keep_bak=False)
    say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
    s.say("O"); s.frames(3)
    check(bak_line() == "BAK__" + bakword, "boot with SAVM_NOCARD=%s: the BAK line says %s: %r" % (nocard, word, bak_line()))
    every_screen(word, "SAVM_NOCARD=" + nocard)

cg = card("card_r3_live")
start(card=cg, keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6); s.tap("A")
good = bak.read_bytes()
for mode, word in (("nocard", "NO CARD"), ("nofat", "CARD NOT FAT32"), ("cardfail", "CARD ERROR")):
    s.say("Z " + mode)
    every_screen(word, "card gone at run time (Z %s)" % mode)
    check(bak.read_bytes() == good, "Z %s: SAVES.BAK untouched" % mode)
    s.say("Z -")
    s.frames(2)
goto(3); s.tap("A"); both(); s.frames(4)
check(screen().startswith("STATE screen=2"), "the card back: RESTORE works again: " + screen())
s.tap("B"); s.frames(2)

# ==== 2. a failed SAVENAME.TXT append: dirty, then written exactly once by the next scan ====
ca = card("card_r3_append", "CHIX=MY CHIX\n")
start(card=ca, fail="append")
r = scan_card()
check("NAMES NOT SAVED" in r and state()["names"]["dirty"] == "1", "append fails: NAMES NOT SAVED, names dirty: " + r)
check((ca / "SAVENAME.TXT").read_text() == "CHIX=MY CHIX\n", "append fails: SAVENAME.TXT byte-identical")
check((ca / "GAMELIST.TXT").exists(), "append fails: GAMELIST.TXT still written")
s.tap("A")
s.say("Z -")                                        # $SAVM_FAIL stays "append": override it with another mode
s.say("Z none")
r = scan_card()
t = (ca / "SAVENAME.TXT").read_text()
check(state()["names"]["dirty"] == "0" and t.count("QQQA=QUUX ALPHA") == 1 and t.count("QQQC=NONASCII") == 1,
      "the next scan writes the waiting names once each: " + repr(t))
check(t.startswith("CHIX=MY CHIX\n"), "the old lines stay first")
s.tap("A")
r = scan_card()
check((ca / "SAVENAME.TXT").read_text() == t and "0 NEW" in r, "a third scan appends nothing: " + r)
s.tap("A")
s.say("Z -")

# ==== 3. a multi-frame scan: every game named, codes on 512-byte and 2 KB / 4 KB edges, sub-folders ====
def chg_at(path, title, code, length, at):
    """A .CHG whose payload is `length` zero bytes with the save call for `code` at byte `at`."""
    payload = bytearray(length)
    c = call(code)
    payload[at:at + len(c)] = c
    head = bytearray(512)
    head[0:4] = b"CHG1"
    head[0x10:0x14] = struct.pack("<I", length)
    head[0x20:0x20 + len(title)] = title.encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(head) + bytes(payload))


cm = card("card_r3_multi", games=False)
games = [  # (path under GAMES, title, code, payload bytes, the call's offset)
    ("EDGE512.CHG", "Edge 512", "EDGA", 3000, 506),
    ("EDGE2K.CHG", "Edge 2K", "EDGB", 9000, 2042),
    ("EDGE4K.CHG", "Edge 4K", "EDGC", 12000, 4090),
    ("LATE.CHG", "Late Code", "LATE", 120000, 119000),
    ("EARLY.CHG", "Early Code", "ERLY", 90000, 2),
    ("SUB/MID.CHG", "Mid Sub", "MIDS", 60000, 30001 & ~1),
    ("OTHER/CARDS/DEEP.CHG", "Deep Game", "DEEP", 70000, 65536),
    ("SMALL.CHG", "Small", "SMLA", 600, 40),
]
for p, title, code, n, at in games:
    chg_at(cm / "GAMES" / p, title, code, n, at)
start(card=cm)
goto(4); s.tap("A")
frames = 0
while screen().startswith("STATE screen=5") and frames < 2000:
    s.frames(10); frames += 10
r = screen()
check("SCAN DONE" in r and "%d GAMES  %d NEW" % (len(games), len(games)) in r, "multi-frame scan: all %d new: %s" % (len(games), r))
check(frames >= 50, "the scan took many frames (%d)" % frames)
t = (cm / "SAVENAME.TXT").read_text() if (cm / "SAVENAME.TXT").exists() else ""
gl = (cm / "GAMELIST.TXT").read_text().splitlines()
for p, title, code, n, at in games:
    check(("%s=%s\n" % (code, title.upper())) in t, "multi-frame scan names %s (%s at %d of %d)" % (code, p, at, n))
    check(("%s %s %s" % (p, code, title)) in gl, "GAMELIST.TXT line for %s" % p)
s.tap("A")

# ==== 4. power cuts in the undo file and a game's .SAV ====
start(keep_bak=False)
for f in (und, utm, bk1, tmpf):
    f.unlink(missing_ok=True)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
s.say("Z cut")                                      # the erase's SAVES.UND write is cut before its rename
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(utm.exists() and not und.exists(), "cut during the undo write: SAVES.UTM alone")
check(pending() == 17 and state()["A"][2] == 5, "the undo write reported failed: ERASE ANYWAY? asked, page kept: " + screen())
s.tap("A"); s.frames(3)                             # NO
s.tap("B"); s.frames(2)
s.say("Z -")
menu(6, 4)
q = ask()
check(screen().startswith("STATE screen=2") and q[:2] == ["A  CHAA  SAME", "B  CHBB  SAME"],
      "UNDO reads the lone SAVES.UTM: %r %s" % (q, screen()))
s.tap("B"); s.frames(2)
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)     # erase A for real
check(state()["A"][0] == 1 and und.exists() and not utm.exists(), "the next erase promotes the .UTM and writes SAVES.UND")
s.tap("A")
menu(6, 4); s.tap("DOWN"); s.tap("A"); s.frames(6)
check(state()["A"][:3] == (3, "CHAA", 5), "UNDO puts page A back (#5)")
s.tap("A")

cs = card("card_r3_sav", games=False)
start(card=cs, keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
goto(0); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)                 # BACK UP THIS GAME: SAVES/CHAA.SAV #5
s.tap("A")
say("W 0 CHAA 1 9 40")
s.say("Z cut")
goto(0); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("DOWN"); s.tap("A"); s.frames(5)   # REPLACE? YES
sv = cs / "SAVES"
check(not (sv / "CHAA.SAV").exists() and (sv / "CHAA.TMP").exists() and (sv / "CHAA.SV1").exists(),
      "cut during a .SAV write: CHAA.TMP and CHAA.SV1 left: %r" % sorted(x.name for x in sv.iterdir()))
s.tap("A")
for _ in range(3): s.tap("B")
s.say("Z -")
say("E 0")
goto(3); s.tap("A"); s.frames(4)
src = st_line("STATE src ")
names = [f.split(":")[0] for f in src.split("files=")[1].split(",")] if "files=" in src else []
check("CHAA.SAV" in names, "the RESTORE list shows the cut-off CHAA.SAV (its .TMP): " + src)
if "CHAA.SAV" in names:
    for _ in range(names.index("CHAA.SAV")): s.tap("DOWN")
    s.tap("A"); s.frames(4)
    check(ask()[:2] == ["A EMPTY -> CHAA", "FROM SAVES/CHAA.SAV"], "restore from the .TMP into the empty page: %r" % ask())
    s.tap("DOWN"); s.tap("A"); s.frames(6)
    check(state()["A"][:3] == (3, "CHAA", 9), "page A holds the cut-off .SAV's record (#9)")
    s.tap("A")

# ==== 5. read-back of restore, erase and undo ====
start(keep_bak=False)
for f in (und, utm, bk1):
    f.unlink(missing_ok=True)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6); s.tap("A")
say("W 0 CHAA 1 7 40")
s.say("Y")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("WRITE FAILED" in screen() and "RESTORED" not in screen(), "restore that reads back wrong: WRITE FAILED: " + screen())
s.tap("A")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("RESTORED" in screen() and state()["A"][2] == 5, "the same restore again without the fault: RESTORED (#5)")
s.tap("A")
s.say("Y")
menu(6, 4); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("FAILED" in screen() and "UNDONE" not in screen(), "an undo that reads back wrong is a failure: " + screen())
s.tap("A")
s.say("Y")
goto(1); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("ERASE FAILED" in screen() and "DID NOT VERIFY" in screen(), "page B erase that reads back wrong: " + screen())
s.tap("A")

# ==== 6. a full round trip: a game's record (say W) -> back up -> erase both -> restore -> identical ====
start(keep_bak=False)
for f in (und, utm, bk1, tmpf):
    f.unlink(missing_ok=True)
say("W 0 STAR 2 41 120", "W 1 CHIX 1 7 64")
p0 = (st_line("STATE page=A"), st_line("STATE page=B"))
check(state()["A"][:4] == (3, "STAR", 41, True) and state()["B"][:4] == (3, "CHIX", 7, True), "two games' records in flash")
menu(2, 6)
check("BACKED UP" in screen() and bak.exists(), "backed up")
s.tap("A")
first = bak.read_bytes()
check(first[16:20] == b"STAR" and first[16 + 256:16 + 260] == b"CHIX", "SAVES.BAK holds both records")
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6); s.tap("A")    # erase A
goto(1); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6); s.tap("A")    # erase B
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1, "both pages erased")
goto(3); s.tap("A"); both(); s.frames(3)
check(ask()[:2] == ["A EMPTY -> STAR", "B EMPTY -> CHIX"], "the restore question: %r" % ask())
s.tap("DOWN"); s.tap("A"); s.frames(6)
check("RESTORED" in screen(), "restored: " + screen())
s.tap("A")
check((st_line("STATE page=A"), st_line("STATE page=B")) == p0, "both pages read as before (game, version, counter, CRC, bytes)")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(6)                  # REPLACE YES: the pages back to the card
s.tap("A")
again = bak.read_bytes()
check(again == first, "the pages backed up again are byte-identical to the first backup (all 512 bytes)")
check(bk1.exists() and bk1.read_bytes() == first, "SAVES.BK1 is the first backup")
check(bak_line() == "BAK__STAR_+_CHIX", "the BAK line: %r" % bak_line())

fails += s.result()
s = None
print("ALL OK" if not fails else f"{fails} FAILED")
raise SystemExit(1 if fails else 0)
