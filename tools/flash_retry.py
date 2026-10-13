#!/usr/bin/env python3
"""Wgrywa firmware, gdy plytka wejdzie w tryb wgrywania (retry w petli).

Plytki ESP32-C3/S3 nie zawsze wchodza w bootloader same, wiec skrypt probuje
do skutku: w momencie wejscia w tryb download zapis od razu rusza.
Wpisy COM w rejestrze Windows zostaja po odlaczeniu plytki ("duchy"), dlatego
port jest najpierw REALNIE otwierany.

Przyklady:
    python tools/flash_retry.py --build c3air/.pio/build/esp32c3-air --chip esp32c3 --flash-size 4MB
    python tools/flash_retry.py --build c3air/.pio/build/esp32s3-air --chip esp32s3 --flash-size 16MB
"""
import argparse
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESPTOOL_PY = os.path.join(os.environ["USERPROFILE"], ".platformio", "penv", "Scripts", "python.exe")


def find_port():
    for p in list_ports.comports():
        try:
            s = serial.Serial(p.device, 115200, timeout=0.3)
            s.close()
            return p.device
        except Exception:
            continue
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="c3air/.pio/build/esp32c3-air")
    ap.add_argument("--chip", default="esp32c3")
    ap.add_argument("--flash-size", default="4MB")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--full", action="store_true",
                    help="zapisz bootloader+partycje+otadata (dla nowej innej plytki)")
    ap.add_argument("--app-only", action="store_true", help="tylko aplikacja (0x10000)")
    ap.add_argument("--erase-nvs", action="store_true",
                    help="wyczyść NVS (0x9000-0xE000) przed zapisem - dla NOWEJ płytki, "
                         "która ma w NVS dane z innego układu partycji")
    args = ap.parse_args()

    build = os.path.join(ROOT, args.build.replace("/", os.sep))
    boot_app0 = os.path.join(os.environ["USERPROFILE"], ".platformio", "packages",
                             "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")

    offsets = [("0x10000", os.path.join(build, "firmware.bin"))]
    if not args.app_only:
        # Klasyczny ESP32 ładuje bootloader z 0x1000 (nie z 0x0, jak C3/S3) -
        # pod złym adresem ROM nie znajduje obrazu i wpada w pętlę
        # "invalid header" / RTCWDT.
        boot_off = "0x1000" if args.chip == "esp32" else "0x0"
        offsets = [(boot_off, os.path.join(build, "bootloader.bin")),
                   ("0x8000", os.path.join(build, "partitions.bin")),
                   ("0xe000", boot_app0)] + offsets

    print("Czekam na tryb wgrywania (max %d s)... chip=%s, obraz=%s"
          % (args.timeout, args.chip, args.build), flush=True)
    if args.erase_nvs:
        print("  (--erase-nvs: najpierw czyszczę NVS 0x9000-0xE000)", flush=True)
    t0 = time.time()
    tries = 0
    in_bootloader = False
    while time.time() - t0 < args.timeout:
        port = find_port()
        if not port:
            time.sleep(2)
            continue
        tries += 1

        # Gdy płytka nie ma podłączonego IO0 do DTR/RTS (kompaktowe 32U), w tryb
        # wgrywania wchodzi się TYLKO trzymając BOOT w chwili resetu. Dlatego:
        #  * albo od razu jesteśmy w bootloaderze (in_bootloader) i nie ruszamy linii,
        #  * albo resetujemy i liczymy, że ktoś trzyma BOOT.
        if args.erase_nvs and not in_bootloader:
            ecmd = [ESPTOOL_PY, "-m", "esptool", "--chip", args.chip, "--port", port,
                    "--before", "default_reset", "--after", "no_reset",
                    "erase_region", "0x9000", "0x5000"]
            r = subprocess.run(ecmd, capture_output=True, text=True, errors="replace")
            eout = (r.stdout or "") + (r.stderr or "")
            # esptool w różnych wersjach opisuje to inaczej ("Erasing flash…",
            # "Erase completed") - sukces rozpoznajemy po BRAKU błędu.
            if "fatal" in eout.lower() or "failed" in eout.lower():
                time.sleep(1)
                continue
            print("  NVS wyczyszczone", flush=True)
            in_bootloader = True

        for before in (("no_reset",) if in_bootloader else ("default_reset", "no_reset")):
            cmd = [ESPTOOL_PY, "-m", "esptool", "--chip", args.chip, "--port", port,
                   "--before", before, "--after", "hard_reset", "--baud", "460800",
                   "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
                   "--flash_size", args.flash_size]
            for off, path in offsets:
                cmd += [off, path]
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
            out = (r.stdout or "") + (r.stderr or "")
            if "Hash of data verified" in out:
                for line in out.splitlines():
                    if any(k in line for k in ("Chip is", "MAC:", "Hash of data verified")):
                        print("  " + line.strip(), flush=True)
                print(">>> WGRANE (proba %d, port %s)" % (tries, port), flush=True)
                return 0
            in_bootloader = False
        time.sleep(4)
    print(">>> NIE UDALO SIE (proby: %d)" % tries, flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
