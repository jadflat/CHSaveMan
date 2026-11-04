"""Drive a CHGame sketch - in the simulator or on the board - with a script:
see chdrivelib.py.

    chgame --sketch <sketch dir> run <script> <outdir>
    python tools/chsim/chdrive.py --device [--port COMx] <script> <outdir>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from chdrivelib import Driver, SerialTransport, SimTransport, main, mask_of  # noqa: E402,F401

if __name__ == "__main__":
    main()
