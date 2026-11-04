"""Build and run a sketch on the PC simulator: the games, and any sketch on CHGfx.

    chgame sim [-D NAME=VAL ...]                                        (from a game's folder)
    python tools/chsim/chsim.py build <sketch> [-D NAME=VAL ...]       -> prints the .exe path
    python tools/chsim/chsim.py run   <sketch> [--frames N] [--gif F | --png DIR] [--input SPEC] ...
    python tools/chsim/chsim.py test                                    CHGfx's own tests

This is the repository's one simulator: every game builds with it (a sketch
may be a folder or a game's, app's or CHGfx example's name: `build CHFour`,
`run GameKit`), and so does any other sketch on CHGfx or the CHGame library.

`build` compiles the sketch's .ino, the .cpp/.c beside it and every .cpp/.c
under its src/ folder,
CHGfx's portable code (every src/*.cpp except CHGfx.cpp, unmodified:
drawing, extras, text effects, palette), the CHGame library (every .cpp
under its src/) when the sketch includes <CHGame.h>, and the host shims in
host/, where chgfx_host.cpp stands in for CHGfx.cpp with a model of the
panel (the wire rate and setup the board measured, rows converted a chunk at
a time, each row landing on a simulated panel as it converts: a torn frame
is torn there, and reported as a BUG).

A sketch on the CHGame library starts in lockstep and is driven through its
debug protocol on stdin/stdout (tools/chsim/chdrivelib.py: that is what
`chgame run` does). `run` here is the free run instead: the sketch runs on
virtual time with no driver, buttons from --input ("F:BTN+BTN,F:" from
presented frame F on), and the presented frames go to PNGs or a GIF timed as
they were shown. It is how CHGfx's examples run, and how to see a game's
panel (host/main.cpp lists the options).

`test` builds CHGfx's extras/tests against the library and this panel
model: about 20,000 checks of the primitives, the clip invariant, the fonts,
the fade and the flush model itself.

A sketch that includes CHSd (<Fat.h> or <SdSpi.h>) also gets CHSd's FAT
reader and, in place of its SPI driver, the pretend card in CHSd's host/
folder: the file named by $CHSD_CARD is in the slot (a *.img as a whole
card, any other file on a FAT16 card made for it; unset: no card).

A game may add shims of its own in <sketch>/tools/chsim/host/: its .cpp
files are compiled too, and one with the same name as a shared shim replaces
it. Its headers come first on the include path, so a header there must not
share a name with one here.

CHGfx is $CHSIM_CHGFX (its src folder) if set, else the repository's own copy
in platform/board/arduino/CHGame/libraries/CHGfx, else the Arduino
sketchbook's libraries/CHGfx, or libraries/CHGfx* (a GitHub zip installs as
CHGfx-main). The sketchbook is $CHSIM_SKETCHBOOK, else what `arduino-cli
config get directories.user` reports, else ~/Documents/Arduino (~/Arduino on
Linux). The CHGame library is found the same way: $CHSIM_CHGAME (its src
folder), else platform/board/arduino/CHGame/libraries/CHGame, else the
sketchbook's libraries/CHGame.

Compiler: $CHSIM_CXX (e.g. "zig c++"), else zig on the PATH, else the
ziglang pip package (`pip install ziglang`), else clang++ or g++.
$CHSIM_FLAGS are added after the usual flags. A memory check of a game
(out-of-bounds writes, uninitialised reads), with valgrind:

    CHSIM_FLAGS="-O0 -g -fno-sanitize=undefined -mcpu=baseline" \\
    CHSIM_WRAP="valgrind -q --error-exitcode=9" \\
        chgame run tools/scripts/<s>.txt out/<s>

(-mcpu=baseline: zig otherwise targets this PC's CPU, whose newest
instructions valgrind may not know; zig's -O0 also turns UBSan on, which
the -fno-sanitize keeps out of the way.) chdrive runs the simulator under
$CHSIM_WRAP when it is set. A report of an uninitialised value in
save::read() is a game's struct padding copied into its save: harmless
(the CRC covers the bytes as stored), though zeroing the struct first
silences it.

The executable is <sketch>/tools/chsim/build/<name>/sim.exe for a sketch
with a tools/chsim folder (every game), else tools/chsim/build/<name>/ here.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent                  # tools/chsim
sys.path.insert(0, str(HERE.parent))
import paths  # noqa: E402
VENDORED_CHGFX = HERE.parents[1] / "platform" / "board" / "arduino" / "CHGame" / "libraries" / "CHGfx" / "src"
VENDORED_CHGAME = HERE.parents[1] / "platform" / "board" / "arduino" / "CHGame" / "libraries" / "CHGame" / "src"
VENDORED_CHSD = HERE.parents[1] / "platform" / "board" / "arduino" / "CHGame" / "libraries" / "CHSd"
W = H = 128
FRAME = 8 + W * H * 3          # a record of the free run's frame file


def sketchbook():
    env = os.environ.get("CHSIM_SKETCHBOOK")
    if env:
        return Path(env)
    try:
        r = subprocess.run(["arduino-cli", "config", "get", "directories.user"],
                           capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            return Path(r.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    home = Path.home()
    return home / "Arduino" if sys.platform.startswith("linux") else home / "Documents" / "Arduino"


def chgfx_dir():
    env = os.environ.get("CHSIM_CHGFX")
    if env:
        d = Path(env)
    elif (VENDORED_CHGFX / "CHGfx_draw.cpp").exists():
        d = VENDORED_CHGFX
    else:
        d = sketchbook() / "libraries" / "CHGfx" / "src"
        if not (d / "CHGfx_draw.cpp").exists():
            # A GitHub zip installs as libraries/CHGfx-main (or -1.3.0, ...).
            found = sorted((sketchbook() / "libraries").glob("CHGfx*/src/CHGfx_draw.cpp"))
            if found:
                d = found[-1].parent
    if not (d / "CHGfx_draw.cpp").exists():
        raise SystemExit(f"CHGfx not found at {d}: install the library or set CHSIM_CHGFX")
    return d


def chgame_dir():
    """The CHGame library's src folder: $CHSIM_CHGAME, else this repository's
    platform/board/arduino/CHGame/libraries/CHGame, else the sketchbook's libraries/CHGame."""
    env = os.environ.get("CHSIM_CHGAME")
    d = Path(env) if env else VENDORED_CHGAME
    if not (d / "CHGame.h").exists():
        d = sketchbook() / "libraries" / "CHGame" / "src"
    if not (d / "CHGame.h").exists():
        raise SystemExit(f"the CHGame library was not found at {d}: set CHSIM_CHGAME")
    return d


def uses(sketch, inos, pattern):
    """Does any source of the sketch include something matching `pattern`?"""
    rx = re.compile(pattern)
    files = list(inos) + [p for p in list(sketch.glob("*")) + list((sketch / "src").rglob("*"))
                          if p.suffix in (".cpp", ".c", ".h", ".hpp")]
    return any(rx.search(p.read_text(encoding="utf-8", errors="replace")) for p in files)


def chsd_dir(sketch, inos):
    """CHSd's folder if the sketch includes it, else None: $CHSIM_CHSD, else
    this repository's copy, else the sketchbook's libraries/CHSd."""
    if not uses(sketch, inos, r'#\s*include\s*<(Fat|SdSpi)\.h>'):
        return None
    env = os.environ.get("CHSIM_CHSD")
    d = Path(env) if env else VENDORED_CHSD
    if not (d / "src" / "Fat.h").exists():
        d = sketchbook() / "libraries" / "CHSd"
    if not (d / "src" / "Fat.h").exists():
        raise SystemExit(f"the CHSd library was not found at {d}: set CHSIM_CHSD")
    return d


def find_cxx():
    env = os.environ.get("CHSIM_CXX")
    if env:
        return env.split()
    if shutil.which("zig"):
        return ["zig", "c++"]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        pass
    for c in ("clang++", "g++"):
        if shutil.which(c):
            return [c]
    raise SystemExit("no C++ compiler: set CHSIM_CXX, put zig/clang++/g++ on the PATH, "
                     "or `pip install ziglang`")


def compile_exe(sources, exe, includes=(), defines=()):
    cmd = find_cxx() + ["-std=gnu++17", "-O1", "-g0", "-w", "-DCHSIM", "-DCH32X035", "-DARDUINO=10800"]
    cmd += [f"-I{d}" for d in includes]
    cmd += [f"-D{d}" for d in defines]
    cmd += os.environ.get("CHSIM_FLAGS", "").split()      # after the defaults, so they win
    cmd += [str(s) for s in sources] + ["-o", str(exe)]
    # zig treats .c as C; everything here is compiled as C++ on purpose.
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"chsim build failed ({exe.name})")
    return exe


def build_dir(sketch):
    """Where a sketch's simulator goes: its own tools/chsim/build/<name>/ when
    it has a tools/chsim folder (every game), else tools/chsim/build/<name>/
    here (so CHGfx's examples grow no folder inside the library)."""
    own = sketch / "tools" / "chsim"
    return (own if own.is_dir() else HERE) / "build" / sketch.name


def build(sketch, defines=(), out=None):
    sketch = paths.sketch(sketch)
    name = sketch.name
    chgfx = chgfx_dir()
    bdir = build_dir(sketch)
    bdir.mkdir(parents=True, exist_ok=True)
    own = sketch / "tools" / "chsim" / "host"
    clash = sorted({p.name for p in own.glob("*.h")} & {p.name for p in (HERE / "host").glob("*.h")})
    if clash:
        raise SystemExit(f"{name}: tools/chsim/host/{clash[0]} has the name of a shared shim header")
    shims = {p.name: p for p in (HERE / "host").glob("*.cpp")}
    shims.update({p.name: p for p in own.glob("*.cpp")})
    inos = sorted(sketch.glob("*.ino"))
    main_ino = sketch / f"{name}.ino"
    if main_ino in inos:
        inos.remove(main_ino)
        inos.insert(0, main_ino)
    unit = bdir / "sketch_ino.cpp"
    with open(unit, "w", encoding="utf-8") as f:
        f.write("#include <Arduino.h>\n")
        for ino in inos:
            f.write(f'#line 1 "{ino.as_posix()}"\n')
            f.write(ino.read_text(encoding="utf-8"))
            f.write("\n")
    srcs = [unit]
    srcs += sorted(p for p in sketch.glob("*") if p.suffix in (".cpp", ".c"))      # beside the .ino, as Arduino does
    srcs += sorted(p for p in (sketch / "src").rglob("*") if p.suffix in (".cpp", ".c")) if (sketch / "src").exists() else []
    srcs += sorted(p for p in chgfx.glob("*.cpp") if p.name != "CHGfx.cpp")
    includes = [d for d in (own, HERE / "host") if d.is_dir()] + [sketch, chgfx]
    if uses(sketch, inos, r'#\s*include\s*<(CHGame\.h|chgame/)'):
        chgame = chgame_dir()
        srcs += sorted(p for p in chgame.rglob("*") if p.suffix in (".cpp", ".c"))
        includes.append(chgame)
    chsd = chsd_dir(sketch, inos)
    if chsd:
        # The FAT reader as it is; SdSpi.cpp compiles to nothing under
        # CHSIM, and host/sd_host.cpp is the card.
        srcs += sorted((chsd / "src").glob("*.cpp"))
        shims.setdefault("sd_host.cpp", chsd / "host" / "sd_host.cpp")
        includes += [chsd / "src", chsd / "host"]
    srcs += [shims[n] for n in sorted(shims)]
    exe = Path(out) if out else bdir / "sim.exe"
    return compile_exe(srcs, exe, includes, defines)


# ---------------------------------------------------------------- the free run

def read_frames(path):
    data = Path(path).read_bytes()
    for off in range(0, len(data) - FRAME + 1, FRAME):
        n, ms = struct.unpack_from("<II", data, off)
        yield n, ms, data[off + 8: off + FRAME]


def to_image(rgb, scale):
    from PIL import Image
    im = Image.frombytes("RGB", (W, H), rgb)
    return im.resize((W * scale, H * scale), Image.NEAREST) if scale != 1 else im


def run(sketch, defines=(), frames=300, every=1, start=0, scale=3, png=None, gif=None, input_spec=None,
        cost=False, max_seconds=None):
    """Build and free-run a sketch; save its presented frames as PNGs and/or a GIF."""
    sketch = paths.sketch(sketch)
    exe = build(sketch, defines)
    frames_file = build_dir(sketch) / "frames.bin"
    cmd = [str(exe), "--frames", str(frames), "--every", str(every), "--start", str(start)]
    if png or gif:
        cmd += ["--out", str(frames_file)]
    if input_spec:
        cmd += ["--input", input_spec]
    if cost:
        cmd.append("--cost")
    if max_seconds is not None:
        cmd += ["--max-seconds", str(max_seconds)]
    r = subprocess.run(cmd)
    if png or gif:
        imgs = [(n, ms, to_image(rgb, scale)) for n, ms, rgb in read_frames(frames_file)]
        if png:
            out = Path(png)
            out.mkdir(parents=True, exist_ok=True)
            for n, _, im in imgs:
                im.save(out / f"frame{n:05d}.png")
            print(f"chsim: {len(imgs)} PNGs in {out}", file=sys.stderr)
        if gif and imgs:
            dur = [max(20, (imgs[i + 1][1] - imgs[i][1])) for i in range(len(imgs) - 1)] + [100]
            Path(gif).parent.mkdir(parents=True, exist_ok=True)
            imgs[0][2].save(gif, save_all=True, append_images=[im for _, _, im in imgs[1:]], duration=dur, loop=0)
            print(f"chsim: {gif} ({len(imgs)} frames)", file=sys.stderr)
    return r.returncode


# ---------------------------------------------------------------- CHGfx's tests

def test():
    """CHGfx's own tests (its extras/tests) against the library and the panel model here."""
    chgfx = chgfx_dir()
    tdir = chgfx.parent / "extras" / "tests"
    if not tdir.is_dir():
        raise SystemExit(f"{tdir}: CHGfx's tests not found")
    bdir = HERE / "build" / "tests"
    bdir.mkdir(parents=True, exist_ok=True)
    exe = bdir / "tests.exe"
    srcs = sorted(tdir.glob("*.cpp")) + sorted(p for p in chgfx.glob("*.cpp") if p.name != "CHGfx.cpp")
    srcs.append(HERE / "host" / "chgfx_host.cpp")
    compile_exe(srcs, exe, includes=[tdir, HERE / "host", chgfx])
    return subprocess.run([str(exe)]).returncode


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="build a sketch's simulator")
    b.add_argument("sketch")
    b.add_argument("-D", dest="defines", action="append", default=[])
    b.add_argument("-o", dest="out")
    r = sub.add_parser("run", help="free-run a sketch; frames to PNGs or a GIF")
    r.add_argument("sketch")
    r.add_argument("-D", dest="defines", action="append", default=[])
    r.add_argument("--frames", type=int, default=300)
    r.add_argument("--every", type=int, default=1)
    r.add_argument("--start", type=int, default=0)
    r.add_argument("--scale", type=int, default=3)
    r.add_argument("--png")
    r.add_argument("--gif")
    r.add_argument("--input")
    r.add_argument("--cost", action="store_true")
    r.add_argument("--max-seconds", type=float)
    sub.add_parser("test", help="CHGfx's own tests")
    a = ap.parse_args(argv)
    if a.cmd == "build":
        print(build(a.sketch, a.defines, a.out))
        return 0
    if a.cmd == "run":
        return run(a.sketch, a.defines, a.frames, a.every, a.start, a.scale, a.png, a.gif, a.input, a.cost,
                   a.max_seconds)
    return test()


if __name__ == "__main__":
    sys.exit(main())
