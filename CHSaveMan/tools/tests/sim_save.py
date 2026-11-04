"""CHSaveMan simulator tests: pages, back up, erase, restore, names, the card scan, card faults.
    python tools/tests/sim_save.py

Each group starts its own simulator with its own environment (sdio_sim.cpp reads $SAVM_BAK, $SAVM_CARD,
$SAVM_NOCARD and $SAVM_FAIL). Card folders are made under tools/tests/out/."""
import os
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for up in ROOT.parents:
    if (up / "chgame" / "tools" / "chsim" / "simsave.py").exists():
        sys.path.insert(0, str(up / "chgame" / "tools" / "chsim"))
        break
from simsave import Session  # noqa: E402

OUT = ROOT / "tools" / "tests" / "out"
OUT.mkdir(parents=True, exist_ok=True)
bak = OUT / "test.bak"
QCMD = "G"   # the hook's state query (Q and T are built-in sim commands)
fails = 0
s = None


def start(card=None, nocard=None, fail=None, keep_bak=True, offer=False):
    """A fresh simulator with this environment."""
    global s, fails
    if s:
        fails += s.result()
    if not keep_bak:
        bak.unlink(missing_ok=True)
        Path(str(bak) + ".tmp").unlink(missing_ok=True)
    os.environ["SAVM_BAK"] = str(bak)
    os.environ["SAVM_CARD"] = str(card or (OUT / "nocard_dir"))
    for k, v in (("SAVM_NOCARD", nocard), ("SAVM_FAIL", fail), ("SAVM_OFFER", "1" if offer else None)):
        if v:
            os.environ[k] = v
        else:
            os.environ.pop(k, None)
    print(f"-- card={Path(os.environ['SAVM_CARD']).name} nocard={nocard} fail={fail}", flush=True)
    s = Session(ROOT)
    s.frames(3)
    return s


def say(*cmds):
    """Debug commands that write pages are done after gfx_wait: give them a frame."""
    for c in cmds:
        s.say(c)
    s.frames(2)


def state():
    """{'A': (kind, magic, seq, valid, spare, name), 'names': {...}, 'screen': str}."""
    out = {}
    for pg in "AB":
        s.prefix = f"STATE page={pg} "
        r = s.say(QCMD)
        f = dict(x.split("=", 1) for x in r.split()[1:])
        out[pg] = (int(f["kind"]), f["magic"], int(f["seq"]), f["valid"] == "1", f["spare"] == "1", f.get("name", ""))
    s.prefix = "STATE names="
    out["names"] = dict(x.split("=", 1) for x in s.say(QCMD).split()[1:])
    s.prefix = "STATE screen="
    out["screen"] = s.say(QCMD)
    s.prefix = "STATE"
    return out


def screen():
    return state()["screen"]


def ask():
    """The restore question's lines: [page A, page B, warning] ('' when no question is up)."""
    s.prefix = "STATE ask="
    r = s.say(QCMD) or "STATE ask="
    s.prefix = "STATE"
    return r[len("STATE ask="):].split("|") if "|" in r else []


def goto(n):
    for _ in range(7): s.tap("UP")
    for _ in range(n): s.tap("DOWN")


def both(k=0):
    """Back up / restore open the page chooser (screen 7; restore after its file list, screen 9): pick k
    (0 both, 1 only A, 2 only B)."""
    s.frames(1)
    if screen().startswith("STATE screen=9"):      # RESTORE: the list of save files first; SAVES.BAK is row 0
        s.tap("A"); s.frames(2)
    if screen().startswith("STATE screen=7"):
        for _ in range(k): s.tap("DOWN")
        s.tap("A")


def menu(n, frames=6, k=0):
    """Pick main-menu line n (0,1 pages; 2 back up; 3 restore; 4 find names; 5 games list; 6 undo)."""
    goto(n); s.tap("A")
    if n in (2, 3): both(k)
    s.frames(frames)


def check(ok, what):
    s.check(ok, what)


# ---- fake .CHG files ----

def call(code):
    """lui a0,%hi(code); c.li a1,1; addi a0,a0,%lo(code); jal ra,+0x100: a save::read/write call (CALL)."""
    v = struct.unpack("<I", code.encode())[0]
    hi = (v + 0x800) & 0xFFFFF000
    lo = (v - hi) & 0xFFF
    return (struct.pack("<I", hi | 10 << 7 | 0x37) + struct.pack("<H", 0x4585) +
            struct.pack("<I", lo << 20 | 10 << 15 | 10 << 7 | 0x13) + struct.pack("<I", 0x100 << 20 | 1 << 7 | 0x6F))


def chg(path, title, codes, pad=0):
    """pad: extra zero bytes after the code calls (a long game: the scan then takes several frames)."""
    payload = (b"".join(call(c) + bytes(80) for c in codes) or bytes(80)) + bytes(pad)
    head = bytearray(512)
    head[0:4] = b"CHG1"
    head[0x10:0x14] = struct.pack("<I", len(payload))
    t = title if isinstance(title, bytes) else title.encode()
    head[0x20:0x20 + len(t)] = t
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(head) + payload)


def card(name, names_txt=None, games=True):
    d = OUT / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    if games:
        chg(d / "GAMES" / "QUUX.CHG", "Quux Alpha", ["QQQA"])
        chg(d / "GAMES" / "SUB" / "CHIX.CHG", "CHix", ["CHIX"])               # in the table: known
        chg(d / "GAMES" / "NONASCII.CHG", "あい".encode("utf-8"), ["QQQC"])   # no usable title
        chg(d / "GAMES" / "TWO.CHG", "Two Codes", ["QQD1", "QQD2"])           # two equal codes: unsure
        chg(d / "GAMES" / "NOSAVE.CHG", "No Save", [])
    if names_txt is not None:
        (d / "SAVENAME.TXT").write_bytes(names_txt.encode())
    return d


def scan_card(frames=40):
    menu(4, frames)
    return screen()


# ==== 1. pages, backup, erase, restore (the 0.5 tests) ====
start(keep_bak=False)
say("E 0", "E 1")
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1, "both pages empty after erase (kind EMPTY)")

say("W 0 CHAA 1 5 40", "W 1 CHAA 1 6 40")
p = state()
check(p["A"][:2] == (3, "CHAA") and p["A"][3], "page A is a valid CHAA record")
check(p["A"][4] and not p["B"][4], "A is the spare, B is the newest")

say("W 0 CHAA 1 5 40", "W 1 CHBB 2 9 60")
p = state()
check(not p["A"][4] and not p["B"][4], "different games: neither is a spare")
check(p["B"][1] == "CHBB" and p["B"][2] == 9, "page B is CHBB, seq 9")

menu(2)
check(bak.exists() and bak.stat().st_size == 16 + 512, "SAVES.BAK written: 528 bytes")
data = bak.read_bytes()
check(data[:8] == b"CHSVBK01", "SAVES.BAK tag")
check(data[16:20] == b"CHAA" and data[16 + 256:16 + 260] == b"CHBB", "SAVES.BAK holds both pages")
check(not Path(str(bak) + ".tmp").exists(), "the backup's .tmp was renamed")
s.tap("A")

# Erase page A through the menus: page A, A (menu), A (ERASE), DOWN (YES), A.
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["A"][0] == 1 and p["B"][1] == "CHBB", "page A erased, page B untouched")
s.tap("A")

s.tap("DOWN"); s.tap("A"); s.tap("A"); s.tap("A"); s.frames(4)    # B, menu, erase, NO (default)
check(state()["B"][1] == "CHBB", "NO leaves page B alone")
s.tap("B"); s.tap("B")

# A over an empty page: a message, not nothing.
goto(0); s.tap("A"); s.frames(2)
check("NOTHING HERE" in screen(), "A on an empty page says NOTHING HERE")
s.tap("A")

# Restore: SAVES.BAK is read first, then the question, then the writes.
say("E 0", "E 1")
goto(3); s.tap("A"); both(); s.frames(4)
check(screen().startswith("STATE screen=2"), "restore reads SAVES.BAK, then asks (CONFIRM)")
q = ask()
check(q == ["A EMPTY -> CHAA", "B EMPTY -> CHBB", ""], "the question shows each page now -> from SAVES.BAK: %r" % q)
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1, "nothing is written before YES")
s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["A"][:2] == (3, "CHAA") and p["B"][:2] == (3, "CHBB") and p["A"][2] == 5 and p["B"][2] == 9,
      "restore put both records back (seq 5 and 9)")
s.tap("A")

say("E 0", "E 1")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("A"); s.frames(4)       # restore, NO
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1, "restore NO leaves the pages empty")

raw = bytearray(bak.read_bytes()); raw[100] ^= 0xFF; bak.write_bytes(bytes(raw))
goto(3); s.tap("A"); both(); s.frames(4)
p = state()
check(p["A"][0] == 1 and p["B"][0] == 1 and "BAD BACKUP" in p["screen"], "a corrupt SAVES.BAK is refused, before any question")
s.tap("A")

bak.unlink()
goto(3); s.tap("A"); both(); s.frames(4)
check("NO SAVES.BAK" in screen(), "a missing SAVES.BAK is reported")
s.tap("A")

menu(2, 3)
check("NOTHING TO" in screen() and not bak.exists(), "nothing to back up when both pages are empty")
s.tap("A")

# A bad-CRC page: a known game's record stays BAD; an unknown code reads as another format.
say("W 0 CHIX 1 5 40"); say("C 0")            # C reads the page: after the W is done
p = state()
check(p["A"][0] == 3 and not p["A"][3], "C flips a byte: page A is a record with a bad CRC")
menu(2)
check(bak.exists() and bak.read_bytes()[16:20] == b"CHIX", "a bad-CRC page is still backed up")
s.tap("A")

# Restore order: the stale page (the spare, here B) is written first.
say("W 0 CHAA 1 8 40", "W 1 CHAA 1 7 40")
menu(2); s.tap("DOWN"); s.tap("A"); s.frames(4); s.tap("A")   # SAVES.BAK exists: REPLACE? YES
say("W 0 CHAA 1 6 40", "W 1 CHAA 1 5 40")            # older copies of both: both must be rewritten
p = state()
check(p["B"][4] and not p["A"][4], "before the restore B is the spare")
goto(3); s.tap("A"); both(); s.frames(3)
q = ask()
check(q == ["A CHAA -> CHAA", "B CHAA -> CHAA OLD", ""], "SAVES.BAK's older copy shows OLD; same game: no loss: %r" % q)
s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["names"]["writes"] == "BA", "restore writes the stale page B first, then A (got %s)" % p["names"]["writes"])
check(p["A"][2] == 8 and p["B"][2] == 7, "both pages restored")
s.tap("A")
say("W 0 CHAA 1 5 40", "W 1 CHAA 1 6 40")            # now A is the spare
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
check(state()["names"]["writes"] == "AB", "with A the spare, A is written first")
s.tap("A")
say("W 0 CHAA 1 3 40")                                # B already matches: only A is written
goto(3); s.tap("A"); both(); s.frames(3)
check(ask()[1] == "B  CHAA  SAME", "a page that matches SAVES.BAK says SAME: %r" % ask())
s.tap("DOWN"); s.tap("A"); s.frames(6)
check(state()["names"]["writes"] == "A", "a page that already matches is not rewritten")
s.tap("A")
say("W 0 CHIX 1 3 40", "W 1 BECK 1 4 40")             # two other games' saves: both would be lost
goto(3); s.tap("A"); both(); s.frames(3)
q = ask()
check(q == ["A CHIX -> CHAA", "B BECKON -> CHAA OLD", "LOSES 2 GAMES' SAVES"], "two saves lost: %r" % q)
s.tap("B"); s.frames(2)
say("W 1 CHAA 1 9 40")                                # only CHIX is lost (CHAA is in SAVES.BAK)
goto(3); s.tap("A"); both(); s.frames(3)
q = ask()
check(q[2] == "LOSES CHIX SAVE", "a different game's save in a page is named: %r" % q)
s.tap("B"); s.frames(2)
check(state()["A"][1] == "CHIX" and not ask(), "B on the question writes nothing")

# A failed flash write: saving is off, the second page is reported, backup is refused.
say("W 0 CHAA 1 1 40", "W 1 CHAA 1 2 40")
s.say("F")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check("RESTORE FAILED" in p["screen"] and "WRITE FAILED" in p["screen"] and "NOT WRITTEN" in p["screen"],
      "a failed write is reported, and the page not written after it: " + p["screen"])
check(p["A"][0] == 4 and p["B"][0] == 4, "both pages show SAVING IS OFF (kind OFF), not GAME CODE")
s.tap("A")
menu(2, 3)
check("SAVING IS OFF" in screen(), "backup refused when saving is off")
s.tap("A")
goto(0); s.tap("A"); s.frames(2)
check("SAVING IS OFF" in screen(), "A on a page says SAVING IS OFF")
s.tap("A")

# A SAVES.BAK that opens but cannot be read is a card error, not a bad file (and BACK UP does not offer to replace it).
start(fail="read")
say("W 0 CHAA 1 5 40")
goto(3); s.tap("A"); both(); s.frames(4)
check("RESTORE FAILED" in screen() and "CARD ERROR" in screen(), "a failed SAVES.BAK read: CARD ERROR: " + screen())
s.tap("A")
old = bak.read_bytes()
menu(2, 4)
check("BACKUP FAILED" in screen() and "CARD ERROR" in screen() and bak.read_bytes() == old,
      "BACK UP over an unreadable SAVES.BAK: CARD ERROR, file untouched: " + screen())
s.tap("A")

# ==== 2. no card / not FAT ====
start(nocard="1")
say("W 0 CHAA 1 5 40")
menu(2)
check("NO CARD" in screen(), "no card: backup says NO CARD")
s.tap("A")
goto(3); s.tap("A"); both(); s.frames(4)
check("NO CARD" in screen(), "no card: restore says NO CARD")
s.tap("A")
check("NO CARD" in scan_card(), "no card: the scan says NO CARD")
s.tap("A")
start(nocard="fat")
say("W 0 CHAA 1 5 40")
menu(2)
check("CARD NOT FAT32" in screen(), "a card with no FAT volume: CARD NOT FAT32")
s.tap("A")

# ==== 3. SAVENAME.TXT and the scan ====
c1 = card("card1", "# my names\nCHIX=MY CHIX\nJUNK LINE\nQQQB=\n")
apple = b"\x00\x05\x16\x07\x00\x02\x00\x00" + bytes(4088)       # a macOS AppleDouble "._" file
(c1 / "GAMES" / "._QUUX.CHG").write_bytes(apple)
(c1 / "GAMES" / "SUB" / "._CHIX.CHG").write_bytes(apple)
(c1 / "GAMES" / "TINY.CHG").write_bytes(b"CHG1")                     # shorter than a header
start(card=c1)
say("W 0 CHIX 1 5 40", "W 1 QQQA 1 5 40")
p = state()
check(p["A"][5] == "MY_CHIX", "SAVENAME.TXT wins over the built-in name (got %s)" % p["A"][5])
check(p["B"][5] == "QQQA", "an unknown code shows as itself")
r = scan_card()
check("SCAN DONE" in r and "5 GAMES  2 NEW" in r and "1 KNOWN  1 UNSURE" in r, "scan counts (._ and short junk not counted): " + r)
p = state()
check(p["B"][5] == "QUUX_ALPHA", "the scan named QQQA")
t = (c1 / "SAVENAME.TXT").read_text()
check(t.startswith("# my names\nCHIX=MY CHIX\nJUNK LINE\nQQQB=\n"), "lines the app does not use are kept")
check("QQQA=QUUX ALPHA\n" in t and "QQQC=NONASCII\n" in t, "new names appended; a non-ASCII title uses the file name")
check("QQD1" not in t and "QQD2" not in t, "an unsure file (two equal codes) is not learned")
check("TWO.CHG QQD1|QQD2 Two Codes" in (c1 / "GAMELIST.TXT").read_text(), "GAMELIST.TXT lists an unsure game's candidates")
check(not (c1 / "SAVENAME.TMP").exists(), "SAVENAME.TMP renamed")
s.tap("A")
r = scan_card()
check("0 NEW" in r and "3 KNOWN" in r and t == (c1 / "SAVENAME.TXT").read_text(), "a second scan learns nothing new")
s.tap("A")

# B stops a scan.
goto(4); s.tap("A"); s.frames(2); s.tap("B", 1, 1); s.frames(6)
check("SCAN STOPPED" in screen() or "SCAN DONE" in screen(), "B during the scan stops it: " + screen())
s.tap("A")

# .TMP fallback (a write cut off before its rename).
c2 = card("card2", None)
(c2 / "SAVENAME.TMP").write_text("QQQZ=ZED TMP\n")
start(card=c2)
say("W 0 QQQZ 1 5 40")
check(state()["A"][5] == "ZED_TMP", "SAVENAME.TMP is read when SAVENAME.TXT is missing")

# Name list full: 32 names in memory, the 33rd line and later stay in the file.
lines = "".join("N%03d=NAME %d\n" % (i, i) for i in range(34))
c3 = card("card3", lines)
start(card=c3)
say("W 0 N031 1 5 40", "W 1 N033 1 5 40")
p = state()
check(p["A"][5] == "NAME_31" and p["B"][5] == "NAME_33", "every SAVENAME.TXT name is used (no 32-name cap)")
r = scan_card()
check("SCAN DONE" in r and "2 NEW" in r, "34 names in the file: the scan still learns: " + r)
check((c3 / "SAVENAME.TXT").read_text().startswith(lines), "SAVENAME.TXT's 34 lines kept")
s.tap("A")

# SAVENAME.TXT over 1 KB: the first whole lines are read, the scan can learn, names are appended, nothing is lost.
big = "".join("# padding line %02d %s\n" % (i, "x" * 80) for i in range(14)) + "ZZ01=PAST THE CAP\nZZ02=ALSO PAST\n"
assert len(big) > 1024 + 200
c4 = card("card4", big)
start(card=c4)
r = scan_card()
check("SCAN DONE" in r and "2 NEW" in r, "a SAVENAME.TXT over 1 KB still scans and learns: " + r)
t = (c4 / "SAVENAME.TXT").read_text()
check(t.startswith(big), "the big SAVENAME.TXT keeps all its old content (lines past the cap too)")
check("QQQA=QUUX ALPHA\n" in t[len(big):] and "QQQC=NONASCII\n" in t[len(big):], "new names appended after the old content")
check(state()["names"]["dirty"] == "0", "names written (not dirty)")
s.tap("A")

# A scanned code already on a line past the 1 KB window: KNOWN, not appended again (on any scan).
big2 = "".join("# padding line %02d %s\n" % (i, "x" * 80) for i in range(14)) + "QQQA=QUUX FROM FILE\n"
c4b = card("card4b", big2)
start(card=c4b)
r = scan_card()
check("SCAN DONE" in r and "1 NEW" in r and "2 KNOWN" in r, "a code past the window counts KNOWN: " + r)
t = (c4b / "SAVENAME.TXT").read_text()
check(t.count("QQQA=") == 1 and "QQQC=NONASCII\n" in t[len(big2):], "a code past the window is not appended again: " + repr(t[len(big2):]))
s.tap("A")
r = scan_card()
check((c4b / "SAVENAME.TXT").read_text() == t and "0 NEW" in r, "a second scan appends nothing: " + r)
check(state()["names"]["dirty"] == "0", "names not dirty after the search")
s.tap("A")

# A failed names write (io): the names stay dirty, the old file is intact.
c5 = card("card5", "CHIX=MY CHIX\n")
start(card=c5, fail="io")
r = scan_card()
check("SCAN FAILED" in r or "NAMES NOT SAVED" in r, "io fault during a scan: " + r)
check((c5 / "SAVENAME.TXT").read_text() == "CHIX=MY CHIX\n", "the old SAVENAME.TXT is intact")
s.tap("A")
# fail=append: the scan works, only the SAVENAME.TXT append fails: names found, still dirty, file intact.
c6 = card("card6", "CHIX=MY CHIX\n")
start(card=c6, fail="append")
r = scan_card()
check("NAMES NOT SAVED" in r or "SCAN FAILED" in r, "a failed names append is reported: " + r)
check((c6 / "SAVENAME.TXT").read_text() == "CHIX=MY CHIX\n", "failed append: the old SAVENAME.TXT is intact")
check(state()["names"]["dirty"] == "1", "failed append: the learned names stay dirty")
s.tap("A")

# Long games: each takes several scan frames; every one must still be named.
c7 = OUT / "card7"
shutil.rmtree(c7, ignore_errors=True)
for i in range(4):
    chg(c7 / "GAMES" / ("BIG%d.CHG" % i), "Big Game %d" % i, ["LG0%d" % i], pad=40000 + i * 7000)
start(card=c7)
r = scan_card(400)
check("SCAN DONE" in r and "4 GAMES  4 NEW" in r, "long games: scan done, 4 new: " + r)
t = (c7 / "SAVENAME.TXT").read_text()
check(all(("LG0%d=BIG GAME %d\n" % (i, i)) in t for i in range(4)), "all 4 long games named: " + repr(t))
s.tap("A")
start(card=c5, fail="crc")
say("W 0 CHAA 1 5 40")
bak.unlink(missing_ok=True)
menu(2)
check("BACKUP FAILED" in screen() and not bak.exists(), "a backup that does not read back is not renamed")
s.tap("A")

# Backup: straight through with no SAVES.BAK; with one, REPLACE? (NO keeps it, YES replaces it).
start(keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2)
check("BACKED UP" in screen() and bak.exists(), "no SAVES.BAK: the backup goes straight through")
s.tap("A")
old = bak.read_bytes()
say("W 0 CHAA 1 7 40")
menu(2, 4)
check(screen().startswith("STATE screen=2") and bak.read_bytes() == old, "SAVES.BAK exists: REPLACE? asked, file untouched")
s.tap("A"); s.frames(3)                                 # NO is the default
check(bak.read_bytes() == old, "NO keeps SAVES.BAK byte-identical")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5)
new = bak.read_bytes()
check(new != old and new[:8] == b"CHSVBK01" and len(new) == 528 and new[16 + 6] == 7, "YES replaces SAVES.BAK with the new pages")
# Only the .tmp exists (a cut-off write): asks, and after YES a valid SAVES.BAK.
tmpf = Path(str(bak) + ".tmp")
bak.replace(tmpf)
s.tap("A")
menu(2, 4)
check(screen().startswith("STATE screen=2"), "only SAVES.BAK.tmp exists: asks to replace: " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(5)
d = bak.read_bytes() if bak.exists() else b""
check(len(d) == 528 and d[:8] == b"CHSVBK01" and not tmpf.exists(), "after YES the result is a valid SAVES.BAK")
s.tap("A")

# B stops a scan that is still running: many multi-hundred-KB games.
cs = card("card_stop")
for i in range(6):
    chg(cs / "GAMES" / ("BIG%d.CHG" % i), "Big %d" % i, ["BG%02d" % i])
    with open(cs / "GAMES" / ("BIG%d.CHG" % i), "ab") as f:
        f.write(bytes(300 * 1024))
start(card=cs)
goto(4); s.tap("A"); s.frames(4); s.tap("B", 1, 1); s.frames(8)
check("SCAN STOPPED" in screen(), "B during a long scan: SCAN STOPPED: " + screen())
s.tap("A")

# A scan over more than 8 KB of payload spanning many frames still finds every code.
cm = card("card_multi", games=False)
chg(cm / "GAMES" / "LONG.CHG", "Long Game", ["LLLA"])
with open(cm / "GAMES" / "LONG.CHG", "ab") as f:
    f.write(bytes(40 * 1024))
    f.write(call("LLLB"))
chg(cm / "GAMES" / "SHORT.CHG", "Short Game", ["SSSA"])
start(card=cm)
r = scan_card(80)
check("SCAN DONE" in r and "2 GAMES" in r, "multi-frame scan completes: " + r)
tm = (cm / "SAVENAME.TXT").read_text() if (cm / "SAVENAME.TXT").exists() else ""
check("SSSA=SHORT GAME" in tm, "the short game is named: " + repr(tm))
glm = (cm / "GAMELIST.TXT").read_text()
check("LONG.CHG LLLA" in glm and "SHORT.CHG SSSA Short Game" in glm,
      "every game of the multi-frame scan is in GAMELIST.TXT (the long one too): " + repr(glm))
s.tap("A")

# The scan ends with IO: the names it found are still written.
c6 = card("card6", "")
start(card=c6, fail="scan")
r = scan_card()
check("SCAN FAILED" in r and "CARD ERROR" in r and "NEW NAMES SAVED" in r, "scan IO, names saved: " + r)
check("QQQA=QUUX ALPHA" in (c6 / "SAVENAME.TXT").read_text(), "SAVENAME.TXT written after a failed scan")
s.tap("A")

# No GAMES folder.
c7 = card("card7", None, games=False)
start(card=c7)
r = scan_card()
check("NO GAMES FOLDER" in r, "a card without GAMES: " + r)
s.tap("A")

# GAMES LIST: the scan writes GAMELIST.TXT (one line a game, '-' for no code), a second sub-folder level is
# scanned (GAMES/OTHER/CARDS), a third is not; the list screen pages the file from the card.
def list_state():
    s.prefix = "STATE list "
    r = s.say(QCMD)
    s.prefix = "STATE"
    return dict(x.split("=", 1) for x in r.split()[2:])


cl = card("card_list", games=False)
for i in range(12):
    chg(cl / "GAMES" / ("G%02d.CHG" % i), "Game %d" % i, ["GG%02d" % i])
chg(cl / "GAMES" / "NOSAVE.CHG", "No Save", [])
chg(cl / "GAMES" / "OTHER" / "CARDS" / "DEEP.CHG", "Deep Game", ["DEEP"])
chg(cl / "GAMES" / "A" / "B" / "C" / "TOODEEP.CHG", "Too Deep", ["TDEP"])
start(card=cl)
menu(5, 4)
check("NO GAMELIST.TXT" in screen(), "GAMES LIST before any scan: " + screen())
s.tap("A")
r = scan_card(120)
check("SCAN DONE" in r and "14 GAMES" in r, "list card: 14 games (2nd level in, 3rd level out): " + r)
gl = (cl / "GAMELIST.TXT").read_text() if (cl / "GAMELIST.TXT").exists() else ""
lines = gl.splitlines()
games = [x for x in lines if not x.startswith("#")]
check(len(games) == 14 and "G00.CHG GG00 Game 0" in lines and "NOSAVE.CHG - No Save" in lines and
      "OTHER/CARDS/DEEP.CHG DEEP Deep Game" in lines and "TDEP" not in gl, "GAMELIST.TXT lines: " + repr(gl[:200]))
check("# SKIPPED 1 FILES IN 1 DEEP FOLDERS" in lines and "1 SKIPPED" in r, "the folder 3 deep is reported: %r %s" % (lines[-1], r))
lines = games
s.tap("A")
menu(5, 4)
L = list_state()
check(screen().startswith("STATE screen=6") and L["n"] == "14" and L["miss"] == "1" and L["top"] == "0",
      "GAMES LIST: 14 games, 1 missing: %r" % L)
check(L["row0"].replace("_", " ") == " ".join(lines[0].split(" ")[1:]).upper(), "row 0 is the file's first line: " + L["row0"])
s.tap("DOWN"); s.frames(3)
L = list_state()
check(L["top"] == "0" and L["cur"] == "1" and L["rowc"].replace("_", " ") == " ".join(lines[1].split(" ")[1:]).upper(),
      "DOWN moves the cursor one line: %r" % L)
s.tap("RIGHT"); s.frames(3)
check(list_state()["top"] == "3", "RIGHT pages to the end (14 - 11 = 3)")
s.snap(str(OUT / "games_list.png")) if hasattr(s, "snap") else None
s.tap("B"); s.frames(2)
check(screen().startswith("STATE screen=0"), "B goes back to the menu")
# A rescan rewrites the file.
(cl / "GAMES" / "G00.CHG").unlink()
r = scan_card(120)
gl2 = (cl / "GAMELIST.TXT").read_text()
check("13 GAMES" in r and len([x for x in gl2.splitlines() if x[0] != "#"]) == 13 and "G00.CHG" not in gl2,
      "a rescan rewrites GAMELIST.TXT: " + r)
s.tap("A")

# One-page back up and restore: the other page of SAVES.BAK (or of flash) is left as it is.
start(keep_bak=False)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6, k=1)
d = bak.read_bytes() if bak.exists() else b""
check(len(d) == 528 and d[16 + 6] == 5 and d[16 + 256:16 + 512] == b"\xff" * 256,
      "only A with no SAVES.BAK: page A saved, page B blank")
s.tap("A")
menu(2, 6)                                              # both: REPLACE? asked
s.tap("DOWN"); s.tap("A"); s.frames(5)
d = bak.read_bytes()
check(d[16 + 6] == 5 and d[16 + 256 + 6] == 6, "both pages backed up (A #5, B #6)")
s.tap("A")
say("W 0 CHAA 1 9 40", "W 1 CHBB 1 8 40")
menu(2, 4, k=1)
check(screen().startswith("STATE screen=2") and "which=0" in screen(), "only A: REPLACE BACKUP'S A? asked: " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(5)
d = bak.read_bytes()
check("PAGE A BACKED UP" in screen() and d[16 + 6] == 9 and d[16 + 256 + 6] == 6,
      "only A: backup's A is #9, backup's B still #6: " + screen())
s.tap("A")
say("W 0 CHAA 1 13 40", "W 1 CHBB 1 12 40")
menu(3, 4, k=2)
q = ask()
check(len(q) == 3 and q[0].startswith("B ") and q[1] == "PAGE A LEFT AS IT IS", "only B: the question is about B only: %r" % q)
s.tap("DOWN"); s.tap("A"); s.frames(5)
p = state()
check("RESTORED PAGE B" in p["screen"] and p["A"][2] == 13 and p["B"][2] == 6, "only B restored (#6), A untouched (#13): %r" % ((p["A"], p["B"]),))
s.tap("A")
# Restoring one page over a game that is in neither SAVES.BAK page: warned.
say("W 0 CHZZ 1 20 40")
menu(3, 4, k=1)
q = ask()
check(len(q) == 3 and q[2].startswith("LOSES"), "only A over a game not in the backup: LOSES warning: %r" % q)
s.tap("A"); s.frames(3)                                 # NO
check(state()["A"][1] == "CHZZ", "NO leaves page A as it was")


# ==== 0.13: undo, BK1, verify, versions, per-game saves, ignore, views, CSV, ties, cap, faults, power cuts ====
und = Path(str(bak) + ".und")
bk1 = Path(str(bak) + ".bk1")


def st_line(prefix):
    s.prefix = prefix
    r = s.say(QCMD) or ""
    s.prefix = "STATE"
    return r


def det_state():
    r = st_line("STATE det ")
    return dict(x.split("=", 1) for x in r.split()[2:])


def bak_state():
    """{'bak': ..., 'rowA': ..., 'rowB': ...} (spaces as '_')."""
    r = st_line("STATE bak=")
    out = {}
    for part in r[len("STATE "):].split(" "):
        k, v = part.split("=", 1)
        out[k] = v
    return out


def pending():
    return int(dict(x.split("=", 1) for x in screen().split()[1:5])["pending"])


# Undo after an erase and after a restore; undo again swaps back.
start(keep_bak=False)
und.unlink(missing_ok=True); bk1.unlink(missing_ok=True)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(6, 4)
check("NOTHING TO UNDO" in screen(), "UNDO with no SAVES.UND: NOTHING TO UNDO: " + screen())
s.tap("A")
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)          # erase page A, YES
p = state()
check(p["A"][0] == 1 and "UNDO: SAVES.UND" in p["screen"], "erase: page A empty, SAVES.UND written: " + p["screen"])
d = und.read_bytes() if und.exists() else b""
check(len(d) == 528 and d[:8] == b"CHSVBK01" and d[16:20] == b"CHAA" and d[16 + 256:16 + 260] == b"CHBB",
      "SAVES.UND holds both pages as they were before the erase")
s.tap("A")
menu(6, 4)
q = ask()
check(screen().startswith("STATE screen=2") and q[:2] == ["A EMPTY -> CHAA", "B  CHBB  SAME"], "UNDO asks first: %r" % q)
s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["A"][:3] == (3, "CHAA", 5) and "UNDONE" in p["screen"], "UNDO put page A back: " + p["screen"])
s.tap("A")
menu(6, 4); s.tap("DOWN"); s.tap("A"); s.frames(6)
check(state()["A"][0] == 1, "a second UNDO swaps back (page A empty again)")
s.tap("A")
menu(6, 4); s.tap("DOWN"); s.tap("A"); s.frames(6); s.tap("A")
# SAVES.BK1: a backup keeps the one before it.
menu(2, 6)
first = bak.read_bytes()
s.tap("A")
say("W 0 CHAA 1 7 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(bk1.exists() and bk1.read_bytes() == first and bak.read_bytes()[16 + 6] == 7, "REPLACE YES: the old SAVES.BAK is SAVES.BK1")
s.tap("A")
b2 = bak_state()
check(b2["bak"] == "BAK__CHAA_+_CHBB" and b2["rowA"] == "A__CHAA|#7_BAK", "main screen: BAK line and page counter + BAK: %r" % b2)
say("W 0 CHAA 1 8 40")
check(bak_state()["rowA"] == "A__CHAA|#8_NEW", "a page newer than SAVES.BAK says NEW: %r" % bak_state())
# A restore writes SAVES.UND first.
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
d = und.read_bytes()
check(state()["A"][2] == 7 and d[16 + 6] == 8, "restore: SAVES.UND has the pages from before it (A #8), A is #7 now")
s.tap("A")
# A write that does not read back is a failure, not RESTORED / ERASED.
say("W 0 CHAA 1 9 40")
s.say("Y")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("WRITE FAILED" in screen() and "RESTORED" not in screen(), "a page that reads back wrong: WRITE FAILED: " + screen())
s.tap("A")
s.say("Y")
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("ERASE FAILED" in screen() and "DID NOT VERIFY" in screen(), "an erase that reads back wrong: " + screen())
s.tap("A")
# A different record version of the same game: the question shows it.
say("W 0 CHAA 2 3 40", "W 1 CHBB 1 6 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("A")
say("W 0 CHAA 3 4 40")
goto(3); s.tap("A"); both(); s.frames(3)
check(ask()[0] == "A CHAA V3 -> V2", "the restore question shows a version change: %r" % ask())
s.tap("B"); s.frames(2)
# RESTORE lists the save files first: SAVES.BAK, .BK1, .UND (then any .SAV); pick .BK1.
goto(3); s.tap("A"); s.frames(4)
src = st_line("STATE src ")
check(screen().startswith("STATE screen=9") and "files=SAVES.BAK:0,SAVES.BK1:0,SAVES.UND:0" in src, "RESTORE: the file list: " + src)
s.tap("DOWN"); s.tap("A"); s.frames(4)
check(screen().startswith("STATE screen=7"), "a two-page file: the page chooser next")
s.tap("A"); s.frames(4)
s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["A"][2] == 7 and p["A"][4] is False and "SAVES.BK1" in p["screen"], "restored from SAVES.BK1 (A #7): " + p["screen"])
s.tap("A")

# Per-game saves: BACK UP THIS GAME, REPLACE?, the hex name, and restore from GAMES LIST.
cg = card("card_game", games=False)
chg(cg / "GAMES" / "CHAA.CHG", "Game Aa", ["CHAA"])
chg(cg / "GAMES" / "NOSAVE.CHG", "No Save", [])
chg(cg / "GAMES" / "TIE.CHG", "Tie Game", ["CHIX", "QQZ9"])          # a tie with one known code: CHIX
chg(cg / "GAMES" / "MORE" / "READER.CHG", "Card Reader", [])
start(card=cg)
say("W 0 CHAA 1 5 40", "W 1 BECK 1 4 40")
goto(0); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)
sav = cg / "SAVES" / "CHAA.SAV"
d = sav.read_bytes() if sav.exists() else b""
check("GAME BACKED UP" in screen() and len(d) == 272 and d[12] == 1 and d[16:20] == b"CHAA" and d[16 + 6] == 5,
      "BACK UP THIS GAME writes SAVES/CHAA.SAV (272 B, one page): " + screen())
s.tap("A")
say("W 0 CHAA 1 9 40")
goto(0); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(pending() == 9, "a .SAV already there: REPLACE GAME BACKUP? asked: " + screen())
s.tap("A"); s.frames(3)
check(sav.read_bytes()[16 + 6] == 5, "NO keeps the old .SAV")
s.tap("B"); s.frames(2)
goto(0); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(sav.read_bytes()[16 + 6] == 9, "YES replaces it (#9)")
sv1 = cg / "SAVES" / "CHAA.SV1"
check(sv1.exists() and sv1.read_bytes()[16 + 6] == 5, "the replaced .SAV is kept as CHAA.SV1 (#5)")
s.tap("A")
say("W 1 ab-c 1 2 40")
goto(1); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(6)
check((cg / "SAVES" / "61622D63.SAV").exists(), "a code with lower case is saved under its hex name")
s.tap("A")
r = scan_card(60)
check("KNOWN" in r and "0 UNSURE" in r, "a tie with exactly one known code is not UNSURE: " + r)
gl = (cg / "GAMELIST.TXT").read_text()
check("TIE.CHG CHIX Tie Game" in gl and "MORE/READER.CHG - Card Reader" in gl, "the tie is written as the known code: " + repr(gl))
s.tap("A")


def list_rows():
    """The games in GAMELIST.TXT order: [(path, code)]."""
    return [tuple(x.split(" ")[:2]) for x in (cg / "GAMELIST.TXT").read_text().splitlines() if x and x[0] != "#"]


def open_row(path):
    """GAMES LIST, cursor on that game (ALL view), then A: the details."""
    menu(5, 4)
    for _ in range([p for p, _ in list_rows()].index(path)): s.tap("DOWN")
    s.tap("A"); s.frames(4)


def restore_sav(name="CHAA.SAV"):
    """RESTORE, then that .SAV from the file list (0.16: the details no longer restore)."""
    goto(3); s.tap("A"); s.frames(4)
    fl = [f.split(":")[0] for f in st_line("STATE src ").split("files=")[1].split(",")]
    for _ in range(fl.index(name)): s.tap("DOWN")
    s.tap("A"); s.frames(4)


say("E 0", "W 1 BECK 1 4 40")
open_row("CHAA.CHG")
D = det_state()
check(screen().startswith("STATE screen=8") and D["code"] == "CHAA" and D["sav"] == "0" and D["savseq"] == "9" and D["shown"] == "GAME_AA",
      "details: code, its .SAV (#9): %r" % D)
s.tap("B"); s.frames(3); s.tap("B"); s.frames(2)
restore_sav()
q = ask()
check(q == ["A EMPTY -> GAME AA", "FROM SAVES/CHAA.SAV", ""], "a .SAV goes to the empty page: %r" % q)
s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["A"][:3] == (3, "CHAA", 9) and p["B"][1] == "BECK" and "SAVES/CHAA.SAV" in p["screen"], "restored from the .SAV into A: %r" % (p["A"],))
s.tap("A")
# Both pages hold the game: the older one gets the .SAV, with a counter past the newer.
say("W 0 CHAA 1 20 40", "W 1 CHAA 1 21 40")
restore_sav()
check(ask()[0].startswith("A GAME AA -> GAME AA"), "both pages hold the game: the spare (A) is the target: %r" % ask())
s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["A"][:5] == (3, "CHAA", 22, True, False) and p["B"][2] == 21, "the restored record is #22 (past #21), valid, the newest: %r" % (p["A"],))
s.tap("A")
# No free page: asked which, and the loss is named.
say("W 0 CHIX 1 3 40", "W 1 BECK 1 4 40")
restore_sav()
check(screen().startswith("STATE screen=7"), "no empty page: RESTORE INTO WHICH PAGE? " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(3)
q = ask()
check(q[0] == "B BECKON -> GAME AA" and q[2] == "LOSES BECKON SAVE", "into B: the BECKON save is named: %r" % q)
s.tap("DOWN"); s.tap("A"); s.frames(6)
check(state()["B"][1] == "CHAA" and state()["A"][1] == "CHIX", "page B now CHAA, A untouched")
s.tap("A")
# Details of a game in flash: BACK UP ITS SAVE (the first option) writes its .SAV (with the old one as .SV1).
say("W 1 CHAA 1 30 40")
open_row("CHAA.CHG"); s.tap("A"); s.frames(5)
check(pending() == 9, "details: BACK UP ITS SAVE asks to replace the .SAV: " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(5)
check(sav.read_bytes()[16 + 6] == 30, "details: the .SAV is now #30")
s.tap("A")
goto(3); s.tap("A"); s.frames(4)
src = st_line("STATE src ")
files = src.split("files=")[1].split(",")
names = [f.split(":")[0] for f in files]
check("CHAA.SAV" in names and "61622D63.SAV" in names, "the file list has the .SAV files: " + src)
for _ in range(names.index("CHAA.SAV")): s.tap("DOWN")
s.tap("A"); s.frames(4)
q = ask()
check(q[:2] == ["B  GAME AA  SAME", "FROM SAVES/CHAA.SAV"], "a .SAV from the list: the game restore question: %r" % q)
s.tap("B"); s.frames(2)

# Views, IGNORE (kept on the card, survives a rescan), SHOW IT AGAIN, and the CSV export.
menu(5, 4)
L = list_state()
check(L["all"] == "4" and L["miss"] == "2", "list: 4 games, 2 missing (no save code): %r" % L)
s.tap("SELECT"); s.frames(3)
check(list_state()["n"] == "2" and list_state()["mode"] == "1", "SELECT: MISSING ONLY shows 2")
s.tap("SELECT"); s.frames(3)
check(list_state()["n"] == "2" and list_state()["mode"] == "2", "SELECT: SAVES ONLY shows 2")
s.tap("SELECT"); s.tap("SELECT"); s.frames(3)
check(list_state()["mode"] == "0", "SELECT four times: back to ALL")
s.tap("B"); s.frames(2)
open_row("MORE/READER.CHG")
D = det_state()
check(D["path"] == "MORE/READER.CHG" and D["code"] == "-" and D["ign"] == "0", "details of a game with no code: %r" % D)
s.tap("A"); s.frames(4)                                            # IGNORE THIS GAME (first: no .SAV)
ig = cg / "IGNORE.TXT"
check(ig.exists() and "MORE/READER.CHG" in ig.read_text(), "IGNORE.TXT names it")
L = list_state()
check(screen().startswith("STATE screen=6") and L["all"] == "3" and L["miss"] == "1" and L["ign"] == "1",
      "ignored: hidden, not MISSING: %r" % L)
s.tap("B"); s.frames(2)
r = scan_card(60)
s.tap("A")
menu(5, 4)
L = list_state()
check(L["all"] == "3" and L["ign"] == "1", "still ignored after a rescan: %r" % L)
s.tap("START"); s.frames(5)
csv = cg / "GAMELIST.CSV"
t = csv.read_bytes().decode() if csv.exists() else ""
check("SAVED GAMELIST.CSV" in screen() and t.startswith("file,folder,code,title,reason missing\r\n") and
      'READER.CHG,"GAMES/MORE","","Card Reader",ignored; no save code found' in t and
      'NOSAVE.CHG,"GAMES","","No Save",no save code found' in t and 'CHAA.CHG,"GAMES","CHAA","Game Aa",\r\n' in t,
      "START in the list exports GAMELIST.CSV: " + repr(t[:300]))
s.tap("A")
menu(5, 4)
for _ in range(3): s.tap("SELECT")
s.frames(3)
check(list_state()["n"] == "1", "IGNORED ONLY shows it")
s.tap("A"); s.frames(4)
check(det_state()["ign"] == "1", "its details say ignored")
s.tap("A"); s.frames(4)                                            # SHOW IT AGAIN
check("READER" not in ig.read_text() and list_state()["n"] == "0", "SHOW IT AGAIN takes it out of IGNORE.TXT")
(cg / "IGNORE.TXT").write_text("# mine\nnosave.chg\n")           # a hand-written file name, any case
s.tap("SELECT"); s.frames(3)
check(list_state()["all"] == "3" and list_state()["ign"] == "1", "a hand-written IGNORE.TXT line (file name only) counts")
s.tap("B"); s.frames(2)

# More than 32 new codes in one scan: NAME LIST FULL, 32 saved; the next scan finds the rest.
cf = card("card_full", games=False)
for i in range(34):
    chg(cf / "GAMES" / ("F%02d.CHG" % i), "Full %d" % i, ["FL%02d" % i])
start(card=cf)
r = scan_card(200)
t = (cf / "SAVENAME.TXT").read_text()
check("NAME LIST FULL" in r and t.count("=FULL") == 32, "34 new codes: NAME LIST FULL, 32 saved: " + r)
s.tap("A")
r = scan_card(200)
t = (cf / "SAVENAME.TXT").read_text()
check("2 NEW" in r and "32 KNOWN" in r and t.count("=FULL") == 34, "the next scan saves the other 2: " + r)
s.tap("A")
# A names write that fails at run time stays dirty, then is written by the next scan.
cz = card("card_z")
start(card=cz)
s.say("Z names")
r = scan_card()
check("NAMES NOT SAVED" in r and state()["names"]["dirty"] == "1", "Z names: the append fails, names stay dirty: " + r)
s.tap("A")
s.say("Z -")
r = scan_card()
t = (cz / "SAVENAME.TXT").read_text() if (cz / "SAVENAME.TXT").exists() else ""
check(state()["names"]["dirty"] == "0" and "QQQA=QUUX ALPHA" in t and "QQQC=NONASCII" in t, "the next scan writes them: " + repr(t))
s.tap("A")

# Power cut between the old file's removal and the rename: SAVES.TMP alone; it is read and then promoted.
start(keep_bak=False)
tmpf = Path(str(bak) + ".tmp")
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6); s.tap("A")
say("W 0 CHAA 1 7 40")
s.say("Z cut")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5)
check("BACKUP FAILED" in screen() and not bak.exists() and tmpf.exists() and bk1.exists(), "cut: only SAVES.TMP (and .BK1) left")
s.tap("A")
s.say("Z -")
say("W 0 CHAA 1 2 40")
goto(3); s.tap("A"); both(); s.frames(3)
check(ask()[0] == "A CHAA -> CHAA", "the .TMP is read as the backup: %r" % ask())
s.tap("B"); s.frames(2)
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(bak.exists() and not tmpf.exists() and bak.read_bytes()[16 + 6] == 2, "the next backup promotes it and replaces it")
s.tap("A")
# Power cut between the two page writes of a restore: the stale page is written, the newest is intact.
say("W 0 CHAA 1 8 40", "W 1 CHAA 1 7 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("A")
say("W 0 CHAA 1 6 40", "W 1 CHAA 1 5 40")
s.say("U")
goto(3); s.tap("A"); both(); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
p = state()
check(p["B"][2] == 7 and p["A"][2] == 6 and p["A"][3] and p["names"]["writes"] == "B",
      "cut mid-restore: B (stale) written, A (the newest) intact: %r" % ((p["A"], p["B"]),))

# 0.17: unknown data shows its first bytes; .CHG files with no CHG1 header are counted; SAVES.BAK read again after a card error.
cb = card("card_bad", games=False)
chg(cb / "GAMES" / "GOOD.CHG", "Good Game", ["GOOD"])
(cb / "GAMES" / "JUNK.CHG").write_bytes(b"X" * 600)                    # not CHG1: counted
(cb / "GAMES" / "TINY.CHG").write_bytes(b"CHG1")                       # shorter than a header: not counted
(cb / "GAMES" / "._GOOD.CHG").write_bytes(b"\x00\x05\x16\x07" + bytes(4092))   # macOS junk: not counted
start(card=cb, keep_bak=False)
say("H 0 01 02 FF 43 41 42 44 00 11 22")
check(state()["A"][0] == 2, "raw bytes that are not a code: UNKNOWN DATA (kind DAMAGED)")
say("W 1 CABD 1 3 40")
check(state()["B"][:4] == (3, "CABD", 3, True), "an Alien Breed (CABD) record reads as a valid save, not UNKNOWN DATA")
say("E 1")
hx = st_line("STATE page=A").split("hex=")[1]
check(hx == "01.02.FF.43.41.42.44.00.", "the page's first bytes are shown: " + hx)
goto(0); s.tap("A"); s.frames(2)
check(screen().startswith("STATE screen=1"), "A on UNKNOWN DATA opens the page menu (it shows the bytes)")
s.tap("B"); s.frames(2)
r = scan_card(40)
gl = (cb / "GAMELIST.TXT").read_text()
check("1 GAMES" in r and "# NOT CHG1: 1 FILES" in gl, "a .CHG with no CHG1 header is counted in GAMELIST.TXT (junk/tiny not): %r" % gl)
s.tap("A")
say("W 0 GOOD 1 5 40")
menu(2, 6); s.tap("A")
s.say("Z io"); s.say("O"); s.frames(3)
check(bak_state()["bak"] == "BAK__CARD_ERROR", "a failing card: BAK  CARD ERROR: %r" % bak_state())
s.say("Z -")
goto(1); s.tap("A"); s.frames(2); s.tap("A"); s.frames(3)            # any message, then back to the menu
check(bak_state()["bak"].startswith("BAK__GOOD"), "back on the menu after a card error: SAVES.BAK read again: %r" % bak_state())

# One page from SAVES.BAK, .BK1 or .UND: ONLY A / ONLY B; the other flash page stays byte-identical; SAVES.UND first.
start(keep_bak=False)
for f in (bk1, und): f.unlink(missing_ok=True)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6); s.tap("A")                                              # SAVES.BAK: A #5, B #6
say("W 0 CHAA 1 7 40", "W 1 CHBB 1 8 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("A")    # SAVES.BAK: #7 #8, .BK1: #5 #6
say("W 0 CHAA 1 9 40", "W 1 CHBB 1 10 40")


def page_bytes():
    p = state()
    return (p["A"][2], p["B"][2])


def restore_one(row, k):
    """RESTORE, file list row `row`, chooser k (1 only A, 2 only B), YES."""
    goto(3); s.tap("A"); s.frames(4)
    for _ in range(row): s.tap("DOWN")
    s.tap("A"); s.frames(4)
    ok = screen().startswith("STATE screen=7")
    for _ in range(k): s.tap("DOWN")
    s.tap("A"); s.frames(4)
    q = ask()
    s.tap("DOWN"); s.tap("A"); s.frames(6)
    r = screen()
    s.tap("A")
    return ok, q, r


ok, q, r = restore_one(0, 1)
check(ok and q[1] == "PAGE B LEFT AS IT IS" and page_bytes() == (7, 10) and "RESTORED PAGE A" in r,
      "only A from SAVES.BAK: A #7, B still #10: %r %s" % (q, r))
d = und.read_bytes()
check(d[16 + 6] == 9 and d[16 + 256 + 6] == 10, "SAVES.UND written first (A #9, B #10)")
ok, q, r = restore_one(1, 2)                                        # row 1: SAVES.BK1
check(ok and q[1] == "PAGE A LEFT AS IT IS" and page_bytes() == (7, 6) and "SAVES.BK1" in r,
      "only B from SAVES.BK1: B #6, A still #7: %r %s" % (q, r))
pA = st_line("STATE page=A")
ok, q, r = restore_one(2, 2)                                        # row 2: SAVES.UND (A #7 B #10 before the last)
check(ok and page_bytes() == (7, 10) and "SAVES.UND" in r and st_line("STATE page=A") == pA,
      "only B from SAVES.UND: B #10 again, A unchanged: %r %s" % (page_bytes(), r))
say("W 0 CHAA 1 11 40")
pB = st_line("STATE page=B")
ok, q, r = restore_one(0, 1)
check(st_line("STATE page=B") == pB, "the page not restored is byte-identical (its state line unchanged)")

# A bad SAVES.BAK never replaces a good SAVES.BK1 (tester F1).
start(keep_bak=False)
bk1.unlink(missing_ok=True)
say("W 0 CHAA 1 5 40", "W 1 CHBB 1 6 40")
menu(2, 6); s.tap("A")
say("W 0 CHAA 1 7 40")
menu(2, 4); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("A")
good1 = bk1.read_bytes()
raw = bytearray(bak.read_bytes()); raw[100] ^= 0xFF; bak.write_bytes(bytes(raw))
say("W 0 CHAA 1 8 40")
menu(2, 4)
check(pending() == 3, "a bad SAVES.BAK: REPLACE? still asked: " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("A")
check(bk1.read_bytes() == good1 and bak.read_bytes()[16 + 6] == 8, "F1: the bad SAVES.BAK is dropped, SAVES.BK1 kept, the new one written")
# No undo file on an erase of a save: ERASE ANYWAY? (NO by default) (tester F2).
s.say("Z io")
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(pending() == 17 and state()["A"][2] == 8, "F2: undo not written: ERASE ANYWAY? asked, page untouched: " + screen())
s.tap("A"); s.frames(3)
check(state()["A"][2] == 8, "F2: NO keeps the page")
s.tap("B"); s.frames(2)
goto(0); s.tap("A"); s.tap("A"); s.tap("DOWN"); s.tap("A"); s.frames(5); s.tap("DOWN"); s.tap("A"); s.frames(5)
check(state()["A"][0] == 1 and "NO UNDO FILE" in screen(), "F2: YES erases, says NO UNDO FILE: " + screen())
s.tap("A")
s.say("Z -")

# The boot offer (SAVM_OFFER=1 here; debug O runs the boot step again).
start(keep_bak=False, offer=True)
say("W 0 CHAA 1 5 40")
s.say("O"); s.frames(3)
check(screen().startswith("STATE screen=2") and pending() == 14, "pages not in SAVES.BAK: BACK UP NOW? offered: " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(6)
check("BACKED UP" in screen() and bak.exists(), "YES backs up")
s.tap("A")
s.say("O"); s.frames(3)
check(screen().startswith("STATE screen=0"), "pages in SAVES.BAK: no offer")
# START: 3 s (90 frames at 30 fps) leaves; 2.6 s does not.
s.press("START", 80); s.frames(2)
check(screen().startswith("STATE screen=0"), "START held 80 frames: still here")
gone = False
try:
    s.press("START", 95); s.frames(2)
    screen()
except BaseException:
    gone = True
check(gone, "START held 95 frames (3 s at 30 fps): the app leaves")
fails += 1 if s.fails else 0
try:
    s.d.t.close()
except BaseException:
    pass
s = None
start()
fails += s.result()
print("ALL OK" if not fails else f"{fails} FAILED")
raise SystemExit(1 if fails else 0)
