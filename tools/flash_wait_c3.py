#!/usr/bin/env python3
"""Wgrywa firmware c3air na ESP32-C3, gdy tylko plytka pojawi sie na USB.

Powod: wpisy COM w rejestrze Windows zostaja po odlaczeniu plytki ("duchy"),
wiec zwykle listowanie portow podaje numer, ktorego nie da sie otworzyc.
Ten skrypt probuje REALNIE otworzyc port przed wgraniem.

Uruchomienie:  python tools/flash_wait_c3.py [--timeout 600]
"""
import argparse
import os
import subprocess
import sys
import time

from serial.tools import list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "c3air", ".pio", "build", "esp32c3-air")
BOOT_APP0 = os.path.join(
    os.environ["USERPROFILE"], ".platformio", "packages",
    "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
ESPTOOL_PY = os.path.join(os.environ["USERPROFILE"], ".platformio", "penv", "Scripts", "python.exe")


def openable_port():
    """Zwraca pierwszy port, ktory da sie naprawde otworzyc (nie duch z rejestru)."""
    import serial
    for p in list_ports.comports():
        try:
            s = serial.Serial(p.device, 115200, timeout=0.5)
            s.close()
            return p.device, p.description
        except Exception:
            continue
    return None, None


def flash(port):
    cmd = [ESPTOOL_PY, "-m", "esptool", "--chip", "esp32c3", "--port", port,
           "--before", "default_reset", "--after", "hard_reset", "--baud", "460800",
           "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "4MB",
           "0x0", os.path.join(BUILD, "bootloader.bin"),
           "0x8000", os.path.join(BUILD, "partitions.bin"),
           "0xe000", BOOT_APP0,
           "0x10000", os.path.join(BUILD, "firmware.bin")]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return (r.stdout or "") + (r.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    print("Czekam na plytke na USB (max %d s)..." % args.timeout, flush=True)
    t0 = time.time()
    port = None
    while time.time() - t0 < args.timeout:
        port, desc = openable_port()
        if port:
            print("port: %s (%s)" % (port, desc), flush=True)
            break
        time.sleep(2)

    if not port:
        print("BRAK PLYTKI - nie doczekalem sie dzialajacego portu", flush=True)
        return 1

    time.sleep(1.0)
    for before in ("default_reset", "usb_reset", "no_reset"):
        print("--- proba (--before %s) ---" % before, flush=True)
        cmd = [ESPTOOL_PY, "-m", "esptool", "--chip", "esp32c3", "--port", port,
               "--before", before, "--after", "hard_reset", "--baud", "460800",
               "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "4MB",
               "0x0", os.path.join(BUILD, "bootloader.bin"),
               "0x8000", os.path.join(BUILD, "partitions.bin"),
               "0xe000", BOOT_APP0,
               "0x10000", os.path.join(BUILD, "firmware.bin")]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        out = (r.stdout or "") + (r.stderr or "")
        for line in out.splitlines():
            if any(k in line for k in ("Chip is", "MAC:", "Writing at", "Hash of data",
                                       "Leaving", "fatal", "Failed", "Hard resetting")):
                print("   " + line.strip(), flush=True)
        if "Hash of data verified" in out:
            print(">>> WGRANE I ZWERYFIKOWANE", flush=True)
            return 0
        time.sleep(2)

    print(">>> NIE UDALO SIE - ROM plytki nie odpowiada", flush=True)
    return 2


if __name__ == "__main__":
    sys.exit(main())
