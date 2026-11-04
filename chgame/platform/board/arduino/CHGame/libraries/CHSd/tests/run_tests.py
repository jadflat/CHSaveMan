"""Build and run CHSd's host tests: src/Fat.cpp against FAT images made with
tools/fatimg.py (whose layout is the ground truth), host/VCard.h, and
src/SdSpi.cpp's init and read against a model of the card (test_spi.cpp).

    python tests/run_tests.py [--quick]

--quick leaves out the FAT32 images (34 MB each). Compiler: $CHSIM_CXX, else
zig on the PATH, else the zig kept beside the workspace (CH32Sound/.work/zig),
else `python -m ziglang`, clang++ or g++. The SPI test needs a 32-bit
target (the driver gives the DMA addresses as uint32_t), which zig builds on
Windows and x86-64 Linux; elsewhere it is skipped.
"""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = HERE / "build"
sys.path.insert(0, str(ROOT / "tools"))
import fatimg  # noqa: E402


def find_cxx():
    if os.environ.get("CHSIM_CXX"):
        return os.environ["CHSIM_CXX"].split()
    if shutil.which("zig"):
        return ["zig", "c++"]
    work = ROOT.parents[3] / "CH32Sound" / ".work" / "zig"
    for z in sorted(work.glob("zig-*/zig.exe")) + sorted(work.glob("zig-*/zig")):
        return [str(z), "c++"]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        pass
    for c in ("clang++", "g++"):
        if shutil.which(c):
            return [c]
    raise SystemExit("no C++ compiler: set CHSIM_CXX or put zig/clang++/g++ on the PATH")


def spi_target(cxx):
    """The 32-bit target test_spi.cpp is built for, or None."""
    if not any("zig" in c for c in cxx):
        return None
    if platform.system() == "Windows":
        return "x86-windows-gnu"
    if platform.system() == "Linux" and platform.machine().lower() in ("x86_64", "amd64"):
        return "x86-linux-musl"
    return None


def contents(size, seed):
    """What test_fat.cpp's byteAt() expects."""
    return bytes((i * 7 + seed * 13 + (i >> 9)) & 255 for i in range(size))


def sfn(name):
    return fatimg.short_name(name).decode("ascii").replace(" ", "_")


def hide(path, name):
    """Set the hidden bit on a file's real directory entry (the decoys spell
    the same short name but are deleted, labels or long-name parts)."""
    img = bytearray(Path(path).read_bytes())
    key = fatimg.short_name(name) + bytes([fatimg.ATTR_ARCH])
    at = img.find(key)
    assert at >= 0 and at % 32 == 0, name
    img[at + 11] |= fatimg.ATTR_HIDDEN
    Path(path).write_bytes(img)


ROOT_FILES = {"WORDS.DIC": 70000, "PHRASES.BNK": 39424, "TINY.TXT": 5, "EMPTY.DAT": 0, "ONE.BLK": 512}
PACKS = {"ALPHA.CWD": 3000, "BETA.CWD": 700, "GAMMA.CWD": 12000}
OTHER = {"NOTES.TXT": 40, "HIDE.CWD": 600}          # in CHCW, but not packs: one is hidden


def make_card(img, spec, **kw):
    files, seeds = {}, {}
    for i, (name, size) in enumerate(ROOT_FILES.items()):
        files[name] = contents(size, i + 1)
        seeds[name] = i + 1
    for name, size in {**PACKS, **OTHER}.items():
        files[f"CHCW/{name}"] = contents(size, 9)
    files["DOCS/WORDS.DIC"] = b"not the root's"   # the same name in another folder
    lay = fatimg.build_image(str(img), files, label="CHSD TEST", **kw)
    hide(img, "HIDE.CWD")
    lines = ["mount 0"]
    for name in ROOT_FILES:
        runs = lay.runs(name)
        flat = " ".join(f"{lba} {n}" for lba, n in runs)
        lines.append(f"file {sfn(name)} {ROOT_FILES[name]} {seeds[name]} {len(runs)} {flat}".rstrip())
    lines += [f"dir {sfn('CHCW')}", f"dir {sfn('DOCS')}"]
    lines.append(f"match {sfn('CHCW')} ????????CWD {len(PACKS)} " + " ".join(sfn(n) for n in PACKS))
    lines.append(f"match {sfn('CHCW')} ??????????? {len(PACKS) + 1} " + " ".join(sfn(n) for n in [*PACKS, "NOTES.TXT"]))
    lines.append(f"match {sfn('CHCW')} BETA____CWD 1 {sfn('BETA.CWD')}")
    lines.append(f"match {sfn('DOCS')} ????????TXT 0")
    lines += [f"missing {sfn('NOPE.DIC')}", f"missing {sfn('ALPHA.CWD')}"]
    Path(spec).write_text("\n".join(lines) + "\n")
    return lay


def main():
    quick = "--quick" in sys.argv
    BUILD.mkdir(exist_ok=True)
    exe = BUILD / "test_fat.exe"
    cxx = find_cxx()
    flags = ["-std=gnu++17", "-O2", "-Wall", "-Wextra", "-Wno-unknown-pragmas",
             "-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
    cmd = cxx + flags + ["-DCHTEST", str(HERE / "test_fat.cpp"), str(ROOT / "src" / "Fat.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("build failed")
    ok = True

    target = spi_target(cxx)
    if target:
        spi = BUILD / "test_spi.exe"
        r = subprocess.run(cxx + flags + ["-target", target, f"-I{HERE / 'spi'}", f"-I{ROOT / 'src'}",
                                          str(HERE / "test_spi.cpp"), str(ROOT / "src" / "SdSpi.cpp"), "-o", str(spi)],
                           capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit("build failed (test_spi)")
        r = subprocess.run([str(spi)], capture_output=True, text=True)
        last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
        print(f"   {'ok  ' if r.returncode == 0 else 'FAIL'} SPI driver against the card model: {last}")
        if r.returncode:
            print(r.stdout[-2000:] + r.stderr[-2000:])
            ok = False
    else:
        print("   skip SPI driver (needs zig for a 32-bit build)")

    def run(label, *args):
        nonlocal ok
        r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
        last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
        print(f"   {'ok  ' if r.returncode == 0 else 'FAIL'} {label}: {last}")
        if r.returncode:
            print(r.stdout[-2000:] + r.stderr[-2000:])
            ok = False

    img, spec = BUILD / "card.img", BUILD / "card.txt"
    run("no card", "--none")
    run("pretend card (VCard)", "--vcard")
    cases = [("fat16", dict(fs="fat16")),
             ("fat16, no partition table", dict(fs="fat16", mbr=False)),
             ("fat16, long names, decoys, files in pieces", dict(fs="fat16", lfn=True, decoys=True, fragment=3, seed=7)),
             ("fat16, 8-sector clusters, one FAT", dict(fs="fat16", spc=8, nfats=1)),
             ("fat16, partition in slot 3", dict(fs="fat16", part_slot=3))]
    if not quick:
        cases += [("fat32", dict(fs="fat32")),
                  ("fat32, long names, decoys, files and root in pieces",
                   dict(fs="fat32", lfn=True, decoys=True, fragment=4, dir_pieces={"": 3}, seed=3))]
    for label, kw in cases:
        make_card(img, spec, **kw)
        run(label, img, spec)

    # Broken chains: looped, cut short, running on past the file's end.
    lay = make_card(img, spec, fs="fat16", fragment=2, seed=11)
    words = lay.objects["WORDS.DIC"].clusters
    for label, cluster, value in (("FAT loop", words[-1], words[0]),
                                  ("chain cut short", words[len(words) // 2], 0xFFFF),
                                  ("chain runs on", words[-1], words[0] - 1 if words[0] > 3 else 0xFFF0)):
        make_card(img, spec, fs="fat16", fragment=2, seed=11)
        fatimg.set_fat_entry(str(img), lay, cluster, value)
        Path(spec).write_text(f"mount 0\nchain {sfn('WORDS.DIC')} {fatimg.E_CHAIN}\n")
        run(label, img, spec)

    # Cards the reader must refuse, and say why.
    fatimg.make_exfat_stub(str(img))
    spec.write_text(f"mount {fatimg.E_EXFAT}\n")
    run("exFAT", img, spec)
    fatimg.make_exfat_stub(str(img), mbr=False)
    run("exFAT, no partition table", img, spec)
    img.write_bytes(bytes(512 * 64))
    spec.write_text(f"mount {fatimg.E_NOFS}\n")
    run("blank card", img, spec)
    fatimg.build_image(str(img), {"A.TXT": b"x"}, fs="fat16", clusters=4000, unchecked=True)
    run("FAT12-sized volume", img, spec)
    img.unlink()
    print("ALL OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
