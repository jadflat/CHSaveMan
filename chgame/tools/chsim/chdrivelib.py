"""Drive a CHGame sketch - in the simulator or on the board - with a script.

    chgame --sketch <sketch dir> run <script> <outdir>
    python tools/chsim/chdrive.py --device [--port COMx] <script> <outdir>

(chdrive.py runs this module's main(); a game's own driver imports it.)

Any sketch that runs the CHGame library's debug protocol (chgame/Debug.h:
dbg::begin() in setup(), dbg::poll() at the top of loop()) can be driven:
the simulator always has it, a board build has it with
`chgame build --debug`. Both targets speak the same protocol, so
one script gives comparable screenshots from each. --id checks the start
of the sketch's hello line (what dbg::begin() was given).

A game adds script commands of its own by subclassing Driver and
overriding op(name, args, outdir); each game has a
tools/chsim/chdrive.py that does, and runs main(ItsDriver, ident=...).

Script lines (# comments allowed):
    wait N              advance N frames
    step N              N frames one at a time (no catch-up ticks)
    tap BTN[+BTN] [H]   hold for H frames (default 3), then release, then 1 frame
    hold BTN[+BTN]      keep held until `release`
    release
    snap NAME           save NAME.png (3x); all snaps also go to sheet.png
    gif NAME N [EVERY]  record N frames (every EVERY-th) to NAME.gif
    rec start [EVERY] / rec stop NAME   record everything in between to NAME.gif
    free SECONDS        run free (real time) for a while, then lockstep again
    freegif NAME SECONDS EVERY   the same, sampled into a GIF
    say TEXT            send TEXT as a raw protocol line (the sketch's dbg::hook)
    cal                 (simulator) time CHGfx's primitives, so perf estimates device times
    perf [LABEL]        print the PERF line (frame times, stack), or after cal the
                        estimated device render time
    prof                print the PROF line (CHGAME_PROFILE builds)
Buttons: A B UP DOWN LEFT RIGHT START SELECT
"""
import argparse
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent                  # tools/chsim
sys.path.insert(0, str(HERE))


from fbimage import sheet, to_image  # noqa: E402

BUTTONS = {"A": 1, "B": 2, "UP": 4, "DOWN": 8, "LEFT": 16, "RIGHT": 32, "START": 64, "SELECT": 128}


def mask_of(spec):
    m = 0
    for part in spec.upper().split("+"):
        if part:
            m |= BUTTONS[part]
    return m


class SimTransport:
    def __init__(self, exe):
        # $CHSIM_WRAP runs it under another program (valgrind: see chsim.py).
        wrap = os.environ.get("CHSIM_WRAP", "").split()
        self.p = subprocess.Popen(wrap + [str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
        self.bugs = []
        threading.Thread(target=self._err, daemon=True).start()

    def _err(self):
        for line in self.p.stderr:
            t = line.decode("latin-1").rstrip()
            self.bugs.append(t)
            sys.stderr.write(t + "\n")

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.p.stdout.read(n - len(buf))
            if not chunk:
                raise EOFError("simulator exited")
            buf += chunk
        return buf

    def readline(self):
        buf = b""
        while not buf.endswith(b"\n"):
            buf += self.read(1)
        return buf.decode("latin-1").strip()

    def close(self):
        self.p.stdin.close()
        rc = self.p.wait(10)
        return rc


class SerialTransport:
    def __init__(self, port=None):
        sys.path.insert(0, str(HERE.parent))                # tools/serialcap.py
        from serialcap import open_port
        self.s = open_port(port)
        self.s.timeout = 1
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.bugs = []

    def send(self, line):
        try:
            self.s.write((line + "\n").encode())
        except Exception as e:      # serial.SerialTimeoutException: the board is not reading
            raise TimeoutError(f"the board does not take input ({e})")

    def read(self, n):
        buf = b""
        end = time.time() + 10
        while len(buf) < n and time.time() < end:
            buf += self.s.read(n - len(buf))
        if len(buf) < n:
            raise TimeoutError(f"wanted {n} bytes, got {len(buf)}")
        return buf

    def readline(self, timeout=30.0):
        buf = b""
        end = time.time() + timeout
        while not buf.endswith(b"\n"):
            if time.time() > end:
                raise TimeoutError(f"no reply from device (got {buf[-40:]!r})")
            buf += self.s.readline()
        return buf.decode("latin-1").strip()

    def close(self):
        self.s.close()
        return 0


class Driver:
    def __init__(self, t, ident=""):
        self.t = t
        self.ident = ident

    def expect(self, prefix, tries=50):
        for _ in range(tries):
            line = self.t.readline()
            if line.startswith(prefix):
                return line
        raise RuntimeError(f"never saw {prefix!r}")

    def cmd(self, line, prefix="OK"):
        self.t.send(line)
        return self.expect(prefix)

    def handshake(self, tries=20):
        # A freshly uploaded board may still be enumerating; replies sent
        # before the host opened the port are dropped, so ask until answered.
        for _ in range(tries):
            self.t.send("?")
            try:
                line = self.t.readline(1.0) if isinstance(self.t, SerialTransport) else self.t.readline()
                if line and not line.startswith(("OK", "ERR", "HELD")) and line.startswith(self.ident):
                    return line
            except TimeoutError:
                time.sleep(0.3)
        raise RuntimeError("device never answered '?'")

    def frames(self, n):
        rec = getattr(self, "rec", None)
        if rec is None:
            if n > 0:
                self.cmd(f"N {n}")
            return
        # Recording: a frame at a time, keeping every rec_every-th.
        for _ in range(n):
            self.cmd("N 1")
            self.rec_count += 1
            if self.rec_count % self.rec_every == 0:
                rec.append(self.shot())

    def query(self, line, prefix):
        """Send a game command and return its reply line starting with prefix."""
        self.t.send(line)
        got = None
        for _ in range(100):
            reply = self.t.readline()
            if reply.startswith(prefix):
                got = reply.strip()
            elif reply.startswith("OK"):
                return got
            elif reply.startswith(("ERR", "HELD")):
                raise SystemExit(f"game refused: {line}")
        raise RuntimeError(f"no reply to {line}")

    def buttons(self, mask):
        self.cmd(f"K {mask:x}")

    def shot(self):
        self.t.send("S")
        hdr = self.expect("FB ")
        n = int(hdr.split()[2])
        return self.t.read(n)

    def op(self, name, args, outdir):
        """A game's own script command, or its own take on a common one: True
        if handled (override in a subclass)."""
        return False

    def run(self, script, outdir, scale=3):
        outdir = Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        self.handshake()
        self.cmd("L1")
        snaps = []
        for raw in Path(script).read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            op, *args = line.split()
            if getattr(self, "verbose", False):
                print(">", line, flush=True)
            if self.op(op, args, outdir):           # the game's own (or its override)
                continue
            if op == "wait":
                self.frames(int(args[0]))
            elif op == "tap":
                self.buttons(mask_of(args[0]))
                self.frames(int(args[1]) if len(args) > 1 else 3)
                self.buttons(0)
                self.frames(1)
            elif op == "hold":
                self.buttons(mask_of(args[0]))
            elif op == "release":
                self.buttons(0)
            elif op == "snap":
                im = to_image(self.shot(), scale)
                im.save(outdir / f"{args[0]}.png")
                snaps.append((args[0], im))
            elif op == "gif":
                name, count = args[0], int(args[1])
                every = int(args[2]) if len(args) > 2 else 1
                frames = []
                for _ in range(count):
                    frames.append(to_image(self.shot(), 2))
                    self.frames(every)
                frames[0].save(outdir / f"{name}.gif", save_all=True, append_images=frames[1:],
                               duration=int(1000 * every / 60), loop=0)
            elif op == "say":
                self.t.send(" ".join(args))
                for _ in range(10000):
                    line = self.t.readline()
                    if line.startswith("HELD"):
                        # The CPU is searching: game commands wait for it,
                        # and in lockstep it needs frames to finish.
                        self.t.send("N 5")
                        continue
                    if line.startswith("OK ") and line[3:].strip().isdigit():
                        self.t.send("N 5")         # a frame ack, still waiting
                        continue
                    if line.startswith("OK"):
                        break
                    if line.startswith("ERR"):
                        raise SystemExit(f"game refused: {raw.strip()}")
                    print(line)                     # the hook's own reply lines
            elif op == "free":
                # Run in real time for N seconds (device timing is only
                # meaningful free-running), then return to lockstep.
                self.cmd("L0")
                time.sleep(float(args[0]))
                self.cmd("L1")
            elif op == "rec":
                # rec start [EVERY]: record from here (every EVERY-th frame, 3);
                # rec stop NAME: write NAME.gif.
                if args[0] == "start":
                    self.rec, self.rec_count = [], 0
                    self.rec_every = int(args[1]) if len(args) > 1 else 3
                else:
                    shots, self.rec = self.rec, None
                    frames = [to_image(d, 2) for d in shots]
                    frames[0].save(outdir / f"{args[1]}.gif", save_all=True, append_images=frames[1:],
                                   duration=int(1000 * self.rec_every / 60), loop=0)
                    print(f"{args[1]}.gif: {len(frames)} frames", flush=True)
            elif op == "freegif":
                # Free-running (real time, as on the board) for SECONDS, a
                # shot every EVERY seconds into a GIF.
                # A shot waits for the game's next frame.
                name, secs, every = args[0], float(args[1]), float(args[2])
                self.cmd("L0")
                frames, t0 = [], time.time()
                while time.time() - t0 < secs:
                    frames.append(to_image(self.shot(), 2))
                    time.sleep(every)
                self.cmd("L1")
                frames[0].save(outdir / f"{name}.gif", save_all=True, append_images=frames[1:],
                               duration=int(1000 * every), loop=0)
            elif op == "step":
                # One frame at a time, as the free-running game does (N k runs
                # up to three logic ticks per drawn frame, like a slow frame's
                # catch-up).
                for _ in range(int(args[0])):
                    self.frames(1)
            elif op == "cal":
                # (simulator) host time of the primitives the CHGfx benchmark
                # measured on the board -> device ns per host ns, for perf.
                # The board has no Q and needs none: its perf is measured.
                if isinstance(self.t, SerialTransport):
                    print("cal: not needed on the board (perf gives measured times)")
                    continue
                self.t.send("Q")
                vals = [int(v) for v in self.expect("CAL").split()[1:]]
                self.expect("OK")
                device_us = [93, 4, 100, 246, 263]   # benchmark-results.txt, 12 bpp run
                ratios = [d * 1000.0 / max(h, 1) for d, h in zip(device_us, vals)]
                self.ratio = sum(ratios) / len(ratios)
                print(f"calibration: device/host = {self.ratio:.1f} (clear, hline, blit16, text24, circle: "
                      f"{[round(r) for r in ratios]})")
            elif op == "perf":
                # After cal (simulator): the render time as the board would
                # take it; else the PERF line (on the board: real times).
                line = self.cmd("P", "PERF")
                label = " ".join(args)
                kv = dict(f.split("=") for f in line.split()[1:] if "=" in f)
                if getattr(self, "ratio", None) and "pcrnd" in kv:
                    avg = int(kv["pcrnd"]) * self.ratio / 1e6
                    mx = int(kv["pcmax"]) * self.ratio / 1e6
                    print(f"perf {label}: est. device render avg {avg:.1f} ms, max {mx:.1f} ms ({kv['frames']} frames)")
                else:
                    print(f"perf {label}: {line}" if label else line)
            elif op == "prof":
                # (a build without CHGAME_PROFILE answers ERR)
                self.t.send("T")
                line = self.expect(("PROF", "ERR"))
                if line.startswith("ERR"):
                    raise SystemExit("no profiler in this build (CHGAME_PROFILE=1)")
                print(line)
            else:
                raise SystemExit(f"bad script line: {raw}")
        if snaps:
            sh = sheet([im for _, im in snaps], cols=4)
            sh.save(outdir / "sheet.png")
        return snaps


def main(driver=Driver, ident=""):
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--sim", metavar="SKETCH")
    g.add_argument("--device", action="store_true")
    ap.add_argument("--port")
    ap.add_argument("--id", default=ident, help="the start of the sketch's hello line (its answer to '?')")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo each script line")
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("script")
    ap.add_argument("outdir")
    a = ap.parse_args()
    if a.sim:
        from chsim import build
        t = SimTransport(build(a.sim, a.defines))
    else:
        t = SerialTransport(a.port)
    d = driver(t, a.id)
    d.verbose = a.verbose
    try:
        d.run(a.script, a.outdir)
    finally:
        if a.device:
            try:
                d.buttons(0)
                d.cmd("L0")
            except Exception:
                pass
        rc = t.close()
    # BUG: drawing into rows still going out; "==": valgrind's reports
    bugs = [b for b in getattr(t, "bugs", []) if b.startswith(("BUG", "=="))]
    if bugs:
        raise SystemExit(f"{len(bugs)} simulator bug report(s)")
    print(f"ok: {a.outdir}")
    return rc


