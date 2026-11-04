"""The shared part of a game's simulator tests (tools/tests/sim_save.py):
build the simulator, drive it in lockstep, ask the game's debug hook for
its state, count the checks.

    from simsave import Session
    s = Session(ROOT)                       # the game's folder
    s.say("J P")                            # a debug command; returns the STATE line(s) it printed
    s.tap("A")                              # press, hold 3 frames, release, settle 2
    s.frames(40)
    s.power_cycle()                         # the game hook V: power off and on
    s.check(ok, "what was checked")         # prints ok/FAIL, counts
    raise SystemExit(s.result())            # 1 if anything failed

`s.d` is the chdrivelib.Driver for anything else (screenshots, raw commands).
"""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from chdrivelib import Driver, SimTransport, mask_of  # noqa: E402
from chsim import build  # noqa: E402


def ident_of(game):
    f = Path(game) / "tools" / "chsim" / "chdrive.py"
    if not f.exists():
        return ""
    spec = importlib.util.spec_from_file_location(f"chdrive_{Path(game).name}", f)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return getattr(m, "IDENT", "")


class Session:
    def __init__(self, game, prefix="STATE"):
        self.game = Path(game).resolve()
        self.prefix = prefix
        self.d = Driver(SimTransport(build(self.game)), ident_of(self.game))
        self.d.handshake()
        self.d.cmd("L1")
        self.fails = 0

    def say(self, line, parse=None):
        """Send a debug command; the last line starting with `prefix` (parsed
        by `parse` if given), else None. ERR aborts the test."""
        self.d.t.send(line)
        got = None
        for _ in range(100):
            r = self.d.t.readline()
            if r.startswith(self.prefix):
                got = parse(r) if parse else r.strip()
            elif r.startswith("OK"):
                return got
            elif r.startswith("ERR"):
                raise SystemExit(f"refused: {line}")
        raise SystemExit(f"no answer to {line}")

    def frames(self, n):
        self.d.frames(n)

    def buttons(self, mask):
        self.d.buttons(mask)

    def tap(self, btn, hold=3, settle=2):
        self.d.buttons(mask_of(btn))
        self.d.frames(hold)
        self.d.buttons(0)
        self.d.frames(settle)

    def press(self, btn, hold):
        """Hold a button for `hold` frames without the settle (a long press)."""
        self.d.buttons(mask_of(btn))
        self.d.frames(hold)
        self.d.buttons(0)

    def power_cycle(self, settle=20):
        self.say("V")
        self.d.frames(settle)

    def check(self, ok, what):
        print(("ok   " if ok else "FAIL ") + what, flush=True)
        self.fails += not ok
        return ok

    def result(self):
        self.d.t.close()
        return 1 if self.fails else 0
