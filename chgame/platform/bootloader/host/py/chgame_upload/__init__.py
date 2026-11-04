"""chgame-upload in Python: the CHGame bootloader's host side.

The same verbs and flags as the Go tool the board package ships
(platform/bootloader/host/go), so platform.txt's recipes run unchanged
against either; test/protocol/vectors.json keeps the two from drifting.
The repository's tools (`chgame upload`, `chgame uploader`) and the
bootloader's hardware tests use this one.

    python -m chgame_upload probe                 (from platform/bootloader/host/py, or installed)
    chgame uploader probe                         (the repository's entry point)
    chgame-upload probe                           (after `pip install -e platform/bootloader/host/py`)
"""
__version__ = "0.2.0"       # the same number as `version` in ../go/main.go (test_cli.py checks)
