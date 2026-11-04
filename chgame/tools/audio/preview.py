"""Render a game's sound effects and music to WAV on the PC, from the real code.

    chgame audio OUTDIR [--only NAME ...]          (from a game's folder)
    python tools/audio/preview.py <game dir> OUTDIR [--only NAME ...]

Compiles the CHGame library's chgame/Audio.cpp with the game's sounds
(Sounds.cpp beside the .ino, and src/audio/: SOUNDS, its effects in Sfx order, and for a game with
music playSong(Song, bool)) and a model of the piezo timer (host/), then
writes one WAV per effect, and one per song for each music rendering
(arpeggio and lead). The names come from the game's own enums (Sfx and
Song, in Sounds.h/Music.h or src/audio/), in their order. For each it prints how often a sounding tone was cut off mid-cycle
and restarted (audible clicks) and a hash of the pin's waveform, so two
versions of the code can be compared exactly; a song also gets a .log of
its pitch, one line per millisecond.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent                  # tools/audio
REPO = HERE.parents[1]
LIB = Path(os.environ.get("CHGAME_LIB_SRC", REPO / "platform" / "board" / "arduino" / "CHGame" / "libraries" / "CHGame" / "src"))
sys.path.insert(0, str(REPO / "tools" / "chsim"))
from chsim import find_cxx  # noqa: E402

SFX_MS = 2500                   # an effect: long enough for the longest
SONG_MS = 120000                # a song plays to its end; this caps it


def enum_names(text, name):
    m = re.search(r"enum\s+class\s+" + name + r"\s*:\s*\w+\s*\{(.*?)\}", text, re.S)
    if not m:
        return []
    body = re.sub(r"//[^\n]*", "", m.group(1))
    names = [n.split("=")[0].strip() for n in body.split(",")]
    return [n.lower() for n in names if n and n != "COUNT"]


def audio_files(game, suffix):
    """The game's sound sources: Sounds.* and Music.* beside the .ino (where the
    examples keep their code), and anything in src/audio/ (generated scores)."""
    root = [game / f"{n}{suffix}" for n in ("Sounds", "Music")]
    return [p for p in root if p.is_file()] + sorted((game / "src" / "audio").glob(f"*{suffix}"))


def build(game, out, music):
    exe = out / "harness.exe"
    audio = game / "src" / "audio"
    srcs = [HERE / "host" / "harness.cpp", LIB / "chgame" / "Audio.cpp"]
    srcs += audio_files(game, ".cpp")
    cmd = find_cxx() + ["-std=gnu++17", "-O2", "-w", f"-I{HERE / 'host'}", f"-I{LIB}", f"-I{audio}",
                        f"-I{game}"]
    if music:
        cmd.append("-DPREVIEW_MUSIC")
    cmd += [str(s) for s in srcs] + ["-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("build failed")
    return exe


def run(exe, wav, mode, kind, index, ms, log=None):
    args = [str(exe), str(wav), str(mode), kind, str(index), str(ms)] + ([str(log)] if log else [])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr)
    return r.stdout.strip()


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("game", help="the game's folder, or its name")
    p.add_argument("outdir")
    p.add_argument("--only", nargs="*", help="just these effects/songs (by name)")
    a = p.parse_args(argv)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import paths
    game = paths.sketch(a.game)
    headers = "".join(h.read_text(encoding="utf-8") for h in audio_files(game, ".h"))
    sfx = enum_names(headers, "Sfx")
    songs = enum_names(headers, "Song") if "playSong" in headers else []
    if not sfx:
        raise SystemExit(f"{game}: no `enum class Sfx` in Sounds.h or src/audio/")
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    exe = build(game, out, bool(songs))
    want = set(a.only or [])
    for i, name in enumerate(songs):
        if name == "none" or (want and name not in want):
            continue
        for mode, mname in ((1, "arpeggio"), (2, "lead")):
            stem = f"music_{name}_{mname}"
            print(f"{stem:28s}", run(exe, out / f"{stem}.wav", mode, "song", i, SONG_MS, out / f"{stem}.log"))
    for i, name in enumerate(sfx):
        if want and name not in want:
            continue
        stem = f"sfx_{name}"
        print(f"{stem:28s}", run(exe, out / f"{stem}.wav", 1, "sfx", i, SFX_MS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
