"""`python -m chgame_upload ...`, or this file run directly from anywhere."""
import sys
from pathlib import Path

if __package__ in (None, ""):                           # run as a file: find the package beside us
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chgame_upload.cli import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
