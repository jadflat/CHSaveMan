"""A game's description for the shared tools: its tools/game.py.

Every game has one (an absent file means every default below). It is a
plain Python module: UPPERCASE constants and, where a game needs code,
hook functions. The shared `chgame test`, `chgame check` and `chgame
redraw` read it; nothing else does. Unknown UPPERCASE names are an error,
so a typo cannot silently turn a check off.

Host tests (`chgame test`):

    TESTS = {name: spec}            an executable per entry, built from
                                    `spec["sources"]` (game-relative paths,
                                    globs allowed; "lib/..." is the CHGame
                                    library's src/, "chsd/..." CHSd's src/; a
                                    trailing "?" marks a source that may be
                                    missing); or a callable returning it
      spec keys and defaults:  opt "-O2", defines ["CHTEST"], includes []
                               (of "lib", "chsd", "chsd-host"), cwd None
                               ("game" to run in the game's folder), args
                               "pass" (the command line's arguments go to
                               the exe; "none"; "filter": they name the
                               tests to run), optional False (skip the exe
                               when its first source is missing), run True
                               (False: build only, a hook runs it),
                               reference None (dict(module, func,
                               args=["--dump"]): the exe's output lines
                               against module.func()), extra_flags []
    QUICK_ARGS = ["--quick"]        what `chgame check --quick` passes
    before_tests(ctx)               hooks; after_tests(ctx, exes) -> bool
    EXTRA_TEST_SCRIPTS = []         Python scripts run after the exes

Check (`chgame check`), on top of the above:

    PRE_STEPS = [(title, argv)]     Python scripts (game-relative) run first
    before_check(ctx) -> bool       a hook run first
    after_host_tests(ctx, stdout) -> bool
    SKIP_SCRIPTS = ("device_",)     script stems not run in the simulator
    ONCE_SCRIPTS = set()            stems run once (no determinism check);
                                    scripts using free/freegif are, always
    ECHO = ()                       output-line prefixes echoed under a
                                    script, besides "perf" and "calibration"
    CARD = None                     dict(scripts=prefix or set of stems,
                                    file=game-relative path or callable(ctx)
                                    -> Path, build=argv to make the file):
                                    the SD card ($CHSD_CARD) those scripts get
    REDRAW = dict(scripts="tools/scripts/diff/*.txt", ticks=(1, 3),
                  define="CHSIM_FORCE_FULL")
    SIM_TESTS = []                  Python scripts run against the simulator
    BUILD_REQUIRE = []              substrings the device build must print

Builds (`chgame build`, `upload`, `export`, `card`):

    SAVE_PAGES = 2                  save pages the release image must leave
                                    free, or the build fails: two keep the A/B
                                    pair (a save survives a power cut), one
                                    saves without that, none switches saving
                                    off (tools/check_size.py)

    FQBN = None                     the board and options to build with instead
                                    of the release/debug defaults (device.py):
                                    CHSDtoUSB needs USB Serial and -Os
    DEFINES = []                    preprocessor defines the sketch always
                                    builds with, on the board (build.extra_flags)
                                    and in the simulator (-D): CHSDtoSerial's
                                    GFX_CHUNK_ROWS=1

`ctx` (check.Ctx / hosttests.Ctx): game, tools (the repository's tools/),
out (<game>/out), quick, run(argv, env=None), drive(script, outdir,
env=None) -> (ok, text), log(text).
"""
import importlib.util
import sys
from pathlib import Path

DEFAULTS = {
    "TESTS": {},
    "QUICK_ARGS": ["--quick"],
    "EXTRA_TEST_SCRIPTS": [],
    "PRE_STEPS": [],
    "SKIP_SCRIPTS": ("device_",),
    "ONCE_SCRIPTS": set(),
    "ECHO": (),
    "CARD": None,
    "REDRAW": {"scripts": "tools/scripts/diff/*.txt", "ticks": (1, 3), "define": "CHSIM_FORCE_FULL"},
    "SIM_TESTS": [],
    "BUILD_REQUIRE": [],
    "SAVE_PAGES": 2,
    "FQBN": None,
    "DEFINES": [],
}
HOOKS = ("before_tests", "after_tests", "before_check", "after_host_tests")
SPEC_DEFAULTS = {"opt": "-O2", "defines": ["CHTEST"], "includes": [], "cwd": None, "args": "pass",
                 "optional": False, "run": True, "reference": None, "extra_flags": []}


class Config:
    def __init__(self, game, values, hooks):
        self.game = Path(game)
        for k, v in DEFAULTS.items():
            setattr(self, k, values.get(k, v))
        for h in HOOKS:
            setattr(self, h, hooks.get(h))

    def tests(self):
        """The TESTS table with every spec completed."""
        t = self.TESTS() if callable(self.TESTS) else self.TESTS
        out = {}
        for name, spec in t.items():
            s = dict(SPEC_DEFAULTS)
            s.update(spec)
            unknown = set(s) - set(SPEC_DEFAULTS) - {"sources"}
            if unknown or "sources" not in s:
                raise SystemExit(f"{self.game.name}/tools/game.py: TESTS[{name!r}]: "
                                 + (f"unknown keys {sorted(unknown)}" if unknown else "no sources"))
            out[name] = s
        return out


def load(game):
    """The game's config (defaults when it has no tools/game.py)."""
    game = Path(game).resolve()
    f = game / "tools" / "game.py"
    if not f.exists():
        return Config(game, {}, {})
    for p in (game / "tools" / "tests", game / "tools"):
        if str(p) not in sys.path:
            sys.path.insert(0, str(p))
    spec = importlib.util.spec_from_file_location(f"game_{game.name}", f)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    values, hooks = {}, {}
    for k, v in vars(m).items():
        if k.startswith("_"):
            continue
        if k.isupper():
            if k not in DEFAULTS:
                raise SystemExit(f"{f}: unknown setting {k} (known: {', '.join(DEFAULTS)})")
            values[k] = v
        elif k in HOOKS:
            hooks[k] = v
    return Config(game, values, hooks)
