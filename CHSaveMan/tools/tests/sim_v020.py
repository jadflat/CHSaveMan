"""0.20 simulator checks: GAMES LIST's HIDE ALL (START in ALL and MISSING ONLY) and SHOW ALL (START in IGNORED ONLY):
the confirm, the counts, IGNORE.TXT's hand-written lines kept, a rescan keeps them hidden, SHOW ALL keeps only #
lines (no lines left: the file goes), and a big hide stops at the 1 KB the app reads, with whole lines only.
    python CHSaveMan/tools/tests/sim_v020.py
Reuses sim_save.py's helpers by executing its text up to its first test group."""
from pathlib import Path

_src = (Path(__file__).resolve().parent / "sim_save.py").read_text(encoding="utf-8")
exec(compile(_src[:_src.index("# ==== 1. pages")], "sim_save_helpers", "exec"))


def st_line(prefix):
    s.prefix = prefix
    r = s.say(QCMD) or ""
    s.prefix = "STATE"
    return r


def lst():
    return dict(x.split("=", 1) for x in st_line("STATE list ").split()[2:])


def pend():
    return int(dict(x.split("=", 1) for x in screen().split()[1:5])["pending"])


def view(mode):
    """GAMES LIST in view mode (0 all, 1 missing, 2 saves, 3 ignored), from the main menu."""
    menu(5, 6)
    for _ in range((mode - int(lst()["mode"])) % 4):     # the view is kept between visits
        s.tap("SELECT"); s.frames(3)


def run_job(frames=400):
    n = 0
    while screen().startswith("STATE screen=3") and n < frames:
        s.frames(5); n += 5


def ign_lines(d):
    f = d / "IGNORE.TXT"
    return f.read_text().split("\n") if f.exists() else None


def scan(d):
    menu(4, 10)
    n = 0
    while screen().startswith("STATE screen=5") and n < 3000:
        s.frames(10); n += 10
    s.frames(4)
    r = screen()
    s.tap("A"); s.frames(2)
    return r


# ==== 1. HIDE ALL from MISSING ONLY, hand-written lines kept, a rescan, SHOW ALL ====
d = card("card_v020", games=False)
chg(d / "GAMES" / "SAVE1.CHG", "Save One", ["QSV1"])
chg(d / "GAMES" / "SAVE2.CHG", "Save Two", ["QSV2"])
chg(d / "GAMES" / "NOSAVE1.CHG", "No Save One", [])
chg(d / "GAMES" / "NOSAVE2.CHG", "No Save Two", [])
chg(d / "GAMES" / "SUB" / "NOSAVE3.CHG", "No Save Three", [])
chg(d / "GAMES" / "TWO.CHG", "Two Codes", ["QQD1", "QQD2"])            # unsure: in MISSING ONLY too
chg(d / "GAMES" / "KEPT.CHG", "Kept Hidden", [])
(d / "IGNORE.TXT").write_text("# my own list\nKEPT.CHG\n")
start(card=d, keep_bak=False)
r = scan(d)
check("SCAN DONE" in r and "7 GAMES" in r, "scan: " + r)
view(1)
L = lst()
check(L["mode"] == "1" and L["n"] == "4" and L["ign"] == "1", "MISSING ONLY: 4 games (3 no save, 1 unsure), 1 ignored: %r" % L)
s.tap("START"); s.frames(3)
check(screen().startswith("STATE screen=2") and pend() == 18, "START in MISSING ONLY asks (HIDE ALL): " + screen())
s.tap("A"); s.frames(3)                              # NO
check(screen().startswith("STATE screen=6") and ign_lines(d) == ["# my own list", "KEPT.CHG", ""], "NO: back to the list, nothing written")
s.tap("START"); s.frames(3)
s.tap("DOWN"); s.tap("A"); s.frames(2)
run_job()
r = screen()
check("HIDDEN" in r and "4 GAMES" in r, "HIDE ALL: 4 games hidden: " + r)
lines = ign_lines(d) or []
check(lines[:2] == ["# my own list", "KEPT.CHG"], "the hand-written lines stay first: %r" % lines)
got = sorted(x for x in lines[2:] if x)
check(got == ["NOSAVE1.CHG", "NOSAVE2.CHG", "SUB/NOSAVE3.CHG", "TWO.CHG"], "the four games are appended: %r" % got)
s.tap("A"); s.frames(2)
view(0)
L = lst()
check(L["n"] == "2" and L["all"] == "2" and L["ign"] == "5", "ALL: the 2 saving games left, 5 ignored: %r" % L)
s.tap("START"); s.frames(3)
check(pend() == 18, "START in ALL with nothing missing still asks (HIDE 0 GAMES?): " + screen())
s.tap("A"); s.frames(3)                              # NO
check(screen().startswith("STATE screen=6"), "NO: back to the list")
s.tap("B"); s.frames(2)
r = scan(d)
view(3)
L = lst()
check(L["mode"] == "3" and L["n"] == "5" and L["all"] == "2", "after a rescan the 5 stay hidden (IGNORED ONLY): %r" % L)
s.tap("START"); s.frames(3)
check(screen().startswith("STATE screen=2") and pend() == 19, "START in IGNORED ONLY asks (SHOW ALL): " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(6)
r = screen()
check("SHOWN AGAIN" in r and "5 GAMES" in r, "SHOW ALL: 5 games: " + r)
check(ign_lines(d) == ["# my own list", ""], "SHOW ALL keeps only the # line: %r" % ign_lines(d))
s.tap("A"); s.frames(2)
view(0)
L = lst()
check(L["n"] == "7" and L["ign"] == "0", "ALL: all 7 games back: %r" % L)

# ==== 2. HIDE ALL from ALL; SHOW ALL with no # lines removes the file ====
s.tap("START"); s.frames(3)
check(pend() == 18, "START in ALL asks (HIDE ALL): " + screen())
s.tap("DOWN"); s.tap("A"); s.frames(2)
run_job()
check("HIDDEN" in screen() and "5 GAMES" in screen(), "HIDE ALL from ALL: 5 games (4 + KEPT): " + screen())
s.tap("A"); s.frames(2)
(d / "IGNORE.TXT").write_text("\n".join(x for x in ign_lines(d) if not x.startswith("#")))   # drop the comment
view(3)
s.tap("START"); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(6)
check("SHOWN AGAIN" in screen() and not (d / "IGNORE.TXT").exists(), "SHOW ALL with nothing left removes IGNORE.TXT")
s.tap("A"); s.frames(2)

# ==== 3. a big hide: batches over several frames, stops at 1 KB with whole lines ====
d2 = card("card_v020_big", games=False)
for i in range(70):
    chg(d2 / "GAMES" / ("FOLDER%02d" % (i // 10)) / ("NOSAVE%02d.CHG" % i), "No Save %d" % i, [])
start(card=d2, keep_bak=False)
scan(d2)
view(1)
check(lst()["n"] == "70", "70 games missing: %r" % lst())
s.tap("START"); s.frames(3); s.tap("DOWN"); s.tap("A"); s.frames(2)
run_job(2000)
r = screen()
t = (d2 / "IGNORE.TXT").read_bytes()
lines = [x for x in t.decode().split("\n") if x]
check("IGNORE.TXT FULL" in r, "a hide past 1 KB says IGNORE.TXT FULL: " + r)
check(len(t) <= 1024 and all(x.startswith("FOLDER") and x.endswith(".CHG") and len(x) == 21 for x in lines),
      "IGNORE.TXT within 1 KB, every line whole: %d B, %d lines" % (len(t), len(lines)))
check(("%d GAMES" % len(lines)) in r and len(set(lines)) == len(lines), "the count says the %d lines written, no repeats: %s" % (len(lines), r))
s.tap("A"); s.frames(2)
view(0)
check(lst()["ign"] == str(len(lines)), "the list counts them ignored: %r" % lst())

fails += s.result()
s = None
print("ALL OK" if not fails else f"{fails} FAILED")
raise SystemExit(1 if fails else 0)
