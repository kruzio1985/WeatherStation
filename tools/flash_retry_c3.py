#!/usr/bin/env python3
"""Wgrywa aplikacje c3air, gdy plytka wejdzie w tryb wgrywania (retry w petli).

Plytka wchodzi w bootloader tylko przyciskiem BOOT + RESET, wiec skrypt
probuje do skutku - w momencie wejscia w tryb download zapis od razu rusza.

Uruchomienie: python tools/flash_retry_c3.py [--timeout 240]
"""
import argparse
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ESP = os.path.join(os.environ["USERPROFILE"], ".platformio", "penv", "Scripts", "python.exe")
BIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "c3air", ".pio", "build", "esp32c3-air", "firmware.bin")


def find_port():
    for p in list_ports.comports():
        try:
            s = serial.Serial(p.device, 115200, timeout=0.3)
            s.close()
            return p.device
        except Exception:
            continue
    return None


def try_flash(port):
    for before in ("default_reset", "no_reset"):
        cmd = [ESP, "-m", "esptool", "--chip", "esp32c3", "--port", port,
               "--before", before, "--after", "hard_reset", "--baud", "460800",
               "write_flash", "0x10000", BIN]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        out = (r.stdout or "") + (r.stderr or "")
        if "Hash of data verified" in out:
            return True, out
    return False, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    print("Trzymaj BOOT i nacisnij RESET - skrypt czeka na tryb wgrywania...", flush=True)
    t0 = time.time()
    attempt = 0
    while time.time() - t0 < args.timeout:
        port = find_port()
        if not port:
            time.sleep(2)
            continue
        attempt += 1
        ok, out = try_flash(port)
        if ok:
            for line in out.splitlines():
                if any(k in line for k in ("Chip is", "MAC:", "Hash of data verified")):
                    print("  " + line.strip(), flush=True)
            print(">>> APLIKACJA WGRANA (proba %d, port %s)" % (attempt, port), flush=True)
            return 0
        time.sleep(4)

    print(">>> NIE UDALO SIE w %d s (proby: %d)" % (args.timeout, attempt), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
