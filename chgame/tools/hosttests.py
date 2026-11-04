"""Build and run a game's host unit tests, as its tools/game.py describes them.

    chgame test [ARGS ...]                 (from the game's folder)
    python tools/hosttests.py GAME [ARGS ...]

Each entry of TESTS (tools/gamecfg.py has the schema) is compiled with the
simulator's compiler, under UBSan (every error stops the run), warnings on,
into tools/tests/build/<name>.exe, then run. ARGS go to the executables
(`--quick`, `--long`, `--story 3`), or name the tests to run for a game
whose entries say args="filter". A test may hold its output to a Python
reference (reference=), and a game may add work before and after the
executables (before_tests, after_tests, EXTRA_TEST_SCRIPTS).

Exit code 0 when every test passed, 1 otherwise; a game without TESTS says
so and passes.
"""
import glob
import os
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "chsim"))
import gamecfg  # noqa: E402
import paths  # noqa: E402

LIB = paths.LIBRARIES / "CHGame" / "src"
CHSD = paths.LIBRARIES / "CHSd"
WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
        "-Wno-unknown-pragmas"]
SAN = ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
INCLUDES = {"lib": LIB, "chsd": CHSD / "src", "chsd-host": CHSD / "host"}


class Ctx:
    def __init__(self, game, quick=False):
        self.game = Path(game)
        self.tools = TOOLS
        self.out = self.game / "out"
        self.quick = quick

    def run(self, argv, env=None, cwd=None):
        e = dict(os.environ)
        e.pop("CHSD_CARD", None)
        e.update(env or {})
        argv = [str(self.game / a) if isinstance(a, str) and a.startswith("tools/") else str(a) for a in argv]
        return subprocess.run([sys.executable, *argv], capture_output=True, text=True, encoding="utf-8",
                              errors="replace", cwd=cwd or self.game, env=e)

    def log(self, text):
        print(text, flush=True)


def resolve(game, source):
    """A spec source as a list of files: game-relative, "lib/", "chsd/", globs, a trailing "?"."""
    optional = source.endswith("?")
    s = source.rstrip("?")
    if s.startswith("lib/"):
        base, rel = LIB, s[4:]
    elif s.startswith("chsd/"):
        base, rel = CHSD / "src", s[5:]
    else:
        base, rel = game, s
    files = sorted(Path(p) for p in glob.glob(str(base / rel))) if any(c in rel for c in "*?[") else [base / rel]
    files = [f for f in files if f.exists()]
    if not files and not optional:
        raise SystemExit(f"{game.name}/tools/game.py: source {source!r} not found")
    return files


def compile_test(ctx, name, spec):
    from chsim import find_cxx
    srcs = [f for s in spec["sources"] for f in resolve(ctx.game, s)]
    if spec["optional"] and (not srcs or not resolve(ctx.game, spec["sources"][0])):
        return None
    exe = ctx.game / "tools" / "tests" / "build" / f"{name}.exe"
    exe.parent.mkdir(parents=True, exist_ok=True)
    cmd = find_cxx() + ["-std=gnu++17", spec["opt"], *WARN, *SAN]
    cmd += [f"-D{d}" for d in spec["defines"]]
    for inc in spec["includes"]:
        cmd.append(f"-I{INCLUDES[inc]}")
    cmd += spec["extra_flags"] + [str(s) for s in srcs] + ["-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"{name}: build failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return exe


def check_reference(ctx, exe, ref):
    """The exe's dump against the Python reference's lines."""
    sys.path.insert(0, str(ctx.game / "tools" / "tests"))
    mod = __import__(ref["module"])
    d = subprocess.run([str(exe), *ref.get("args", ["--dump"])], capture_output=True, text=True,
                       cwd=ctx.game)
    got = [g for g in d.stdout.splitlines() if g.strip()]
    want = list(getattr(mod, ref["func"])())
    bad = [(g, w) for g, w in zip(got, want) if g != w]
    if len(got) != len(want):
        bad.append((f"{len(got)} lines", f"{len(want)} lines"))
    for g, w in bad[:10]:
        print(f"FAIL reference: got  {g}")
        print(f"                want {w}")
    if bad or d.returncode:
        print(f"reference cross-check: {len(bad)} mismatches")
        return False
    print(f"reference cross-check: {len(want)} lines match ({ref['module']}.{ref['func']})")
    return True


def run_tests(game, argv=()):
    game = paths.sketch(game)
    cfg = gamecfg.load(game)
    argv = list(argv)
    ctx = Ctx(game, quick="--quick" in argv)
    tests = cfg.tests()
    if not tests:
        print(f"{game.name}: host tests: none")
        return 0
    ok = True
    if cfg.before_tests:
        cfg.before_tests(ctx)
    exes = {}
    failed = []
    for name, spec in tests.items():
        exe = compile_test(ctx, name, spec)
        if exe is None:
            continue
        exes[name] = exe
        if not spec["run"]:
            continue
        if spec["args"] == "filter":
            if argv and name not in argv:
                continue
            args = []
            print(f"== {name}", flush=True)
        else:
            args = argv if spec["args"] == "pass" else []
        cwd = game if spec["cwd"] == "game" else None
        code = subprocess.run([str(exe), *args], cwd=cwd).returncode
        if code:
            failed.append(name)
        if spec["reference"] and not check_reference(ctx, exe, spec["reference"]):
            failed.append(name + " (reference)")
    if cfg.after_tests:
        ok &= bool(cfg.after_tests(ctx, exes))
    for script in cfg.EXTRA_TEST_SCRIPTS:
        print(f"== {Path(script).name}", flush=True)
        if subprocess.run([sys.executable, str(game / script)], cwd=game).returncode:
            failed.append(Path(script).name)
    if failed:
        print(f"failed: {' '.join(failed)}")
    return 0 if ok and not failed else 1


def main(game=None, argv=None):
    if game is None:
        args = list(sys.argv[1:] if argv is None else argv)
        if not args:
            raise SystemExit(__doc__)
        game, argv = args[0], args[1:]
    return run_tests(game, argv or [])


if __name__ == "__main__":
    sys.exit(main())
