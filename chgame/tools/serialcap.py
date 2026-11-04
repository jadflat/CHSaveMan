"""Open the CHGame's USB serial port with DTR set and print what it says.

    python serialcap.py [--port COMx] [--seconds 6] [--until TEXT]

With no --port it picks the first 16C0:27DD device. Exits early when --until
text appears. Used by the probes and by device.py.
"""
import argparse
import sys
import time

import serial
import serial.tools.list_ports

VID, PID = 0x16C0, 0x27DD


def find_port(timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        for p in serial.tools.list_ports.comports():
            if p.vid == VID and p.pid == PID:
                return p.device
        time.sleep(0.2)
    return None


def open_port(port=None, timeout=10.0):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        dev = port or find_port(max(0.5, end - time.time()))
        if dev:
            try:
                # A write to a board that is not reading (crashed, or sitting in
                # its bootloader) must give up: a blocked write cannot be
                # killed and holds the port until the board is unplugged.
                s = serial.Serial(dev, 115200, timeout=0.1, write_timeout=2)
                s.dtr = True
                return s
            except serial.SerialException as e:  # port still re-enumerating
                last = e
        time.sleep(0.25)
    raise SystemExit(f"no CHGame serial port ({last})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--until")
    a = ap.parse_args()
    s = open_port(a.port)
    end = time.time() + a.seconds
    seen = ""
    while time.time() < end:
        data = s.read(4096)
        if data:
            txt = data.decode("latin-1")
            sys.stdout.write(txt)
            sys.stdout.flush()
            seen += txt
            if a.until and a.until in seen:
                break
    s.close()


if __name__ == "__main__":
    main()
