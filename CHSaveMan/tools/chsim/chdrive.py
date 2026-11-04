"""Drive CHSaveMan - in the simulator or on the board - with a script.

    cl run <script> <outdir>                   (the simulator)
    python tools/chsim/chdrive.py --device [--port COMx] <script> <outdir>

The CHGame tools' tools/chsim/chdrivelib.py does the driving and has the
common script commands (wait, step, tap, hold, snap, gif, rec, say, perf,
cal ...). This project keeps a copy of those tools in chgame/ beside the
sketch; $CHGAME_ROOT names another CHGame checkout instead.

"""
import os
import sys
from pathlib import Path


def _tools():
    """The CHGame tools: $CHGAME_ROOT/tools, else chgame/tools (or tools/) above this sketch."""
    root = os.environ.get("CHGAME_ROOT")
    cands = [Path(root) / "tools"] if root else []
    for up in Path(__file__).resolve().parents:
        cands += [up / "chgame" / "tools", up / "tools"]
    for c in cands:
        if (c / "chsim" / "chdrivelib.py").exists():
            return c
    raise SystemExit(f"{Path(__file__).name}: the CHGame tools (chgame/tools/chsim/chdrivelib.py) were not found; "
                     "set CHGAME_ROOT to a CHGame checkout")


sys.path.insert(0, str(_tools() / "chsim"))
from chdrivelib import Driver, SerialTransport, SimTransport, main, mask_of  # noqa: E402,F401


class SaveManDriver(Driver):
    def op(self, name, args, outdir):
        return False


DRIVER, IDENT = SaveManDriver, "SAVM"     # what the shared tools load from this file

if __name__ == "__main__":
    main(DRIVER, ident=IDENT)
