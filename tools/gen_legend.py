#!/usr/bin/env python3
# =============================================================================
#  Stacja Pogody - generator legendy czujnikow dla strony www
#  Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
#  Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
# =============================================================================
#  Czyta docs/CZUJNIKI.md i zapisuje data/legend.js (window.CZUJNIKI),
#  ktory rysuje zakladka "Piny" -> sekcja "Legenda czujnikow".
#
#  Uruchomienie (z katalogu projektu):
#      python tools/gen_legend.py
#      python tools/gen_legend.py --check      # tylko statystyki, bez zapisu
#
#  Po kazdej zmianie docs/CZUJNIKI.md uruchom generator i wgraj LittleFS.
# =============================================================================
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "docs" / "CZUJNIKI.md"
OUT = ROOT / "data" / "legend.js"

CAT_RE = re.compile(r"^##\s+(\d+)\.\s+(.+?)\s*$")
HEADER_FIRST = ("czujnik", "czujniki", "modul", "moduł", "element", "pozycja", "hak")
HEADER_SECOND = ("znaczenie", "opis", "uwagi", "metoda")

STATUS_OK = "\u2705"      # zielony ptaszek
STATUS_PLAN = "\U0001f7e1"  # zolta kropka
STATUS_NO = "\u26d4"        # zakaz

BUS_ORDER = ["I2C", "I2S", "1W", "SPI", "UART", "RS485", "ADC", "GPIO", "INT", "INNE"]


def clean(text: str) -> str:
    """Zdejmuje znaczniki markdownu, zostawia zwykly tekst."""
    text = text.strip()
    text = text.replace("**", "")
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    return re.sub(r"\s+", " ", text).strip()


def status_of(name: str) -> str:
    if STATUS_OK in name:
        return "ok"
    if STATUS_PLAN in name:
        return "plan"
    if STATUS_NO in name:
        return "no"
    return "?"


def status_icon(status: str) -> str:
    return {"ok": STATUS_OK, "plan": STATUS_PLAN, "no": STATUS_NO}.get(status, "")


def detect_bus(conn: str, cat: str) -> str:
    t = (conn + " " + cat).lower().replace("\u00b2", "2")
    if "i2s" in t:
        return "I2S"
    if "i2c" in t:
        return "I2C"
    if "1-wire" in t or "1 wire" in t or "onewire" in t:
        return "1W"
    if "rs485" in t:
        return "RS485"
    if "uart" in t:
        return "UART"
    if "spi" in t or "ssi" in t:
        return "SPI"
    if "adc" in t:
        return "ADC"
    if "gpio" in t or "hx711" in t or "przerw" in t or "impuls" in t or "pwm" in t:
        return "GPIO"
    if conn.strip() in ("\u2014", "-", ""):
        return "INT"
    return "GPIO"


def strip_icon(name: str) -> str:
    for ch in (STATUS_OK, STATUS_PLAN, STATUS_NO):
        name = name.replace(ch, "")
    return re.sub(r"\s+", " ", name).strip()


def main() -> int:
    check_only = "--check" in sys.argv
    if not SRC.exists():
        print("brak pliku: %s" % SRC)
        return 1
    lines = SRC.read_text(encoding="utf-8").splitlines()

    rows = []
    cat_no, cat_name = 0, ""
    header_ok = False
    for raw in lines:
        line = raw.rstrip()
        m = CAT_RE.match(line)
        if m:
            cat_no, cat_name = int(m.group(1)), clean(m.group(2))
            header_ok = False
            continue
        if line.startswith("## "):
            cat_no, cat_name, header_ok = 0, "", False
            continue
        if not cat_no or not line.startswith("|"):
            continue
        cells = [clean(c) for c in line.strip().strip("|").split("|")]
        if not cells or not cells[0]:
            continue
        if all(set(c) <= set("-: ") for c in cells):        # separator |---|---|
            continue
        first = cells[0].lower().rstrip(":")
        first_base = re.split(r"[ /]", first)[0]             # "Czujnik / modul" -> "czujnik"
        second = (cells[1].lower() if len(cells) > 1 else "")
        if first_base in HEADER_FIRST and (
            second.startswith("co ") or second in HEADER_SECOND
        ):
            header_ok = True                                 # wiersz naglowka tabeli
            continue
        if not header_ok or first_base.startswith("kolumna"):
            continue
        name = strip_icon(cells[0])
        if not name:
            continue
        what = cells[1] if len(cells) > 1 else ""
        conn = cells[2] if len(cells) > 2 else ""
        par = cells[3] if len(cells) > 3 else ""
        cal = cells[4] if len(cells) > 4 else ""
        log = cells[5] if len(cells) > 5 else ""
        # wiersze-odsylacze ("patrz rozdzial N") nie sa urzadzeniami - pomijamy
        if status_of(cells[0]) == "?" and re.search(
            r"patrz\s+rozdzia", what + " " + conn, re.I
        ):
            continue
        rows.append({
            "c": "%d. %s" % (cat_no, cat_name),
            "n": name,
            "st": status_of(cells[0]),
            "bus": detect_bus(conn, cat_name),
            "what": what,
            "conn": conn,
            "par": par,
            "cal": cal,
            "log": log,
        })

    counts = {}
    buses = {}
    for r in rows:
        counts[r["st"]] = counts.get(r["st"], 0) + 1
        buses[r["bus"]] = buses.get(r["bus"], 0) + 1
    print("wiersze: %d" % len(rows))
    print("status:  " + ", ".join("%s=%d" % (k, counts[k]) for k in sorted(counts)))
    print("bussy:   " + ", ".join("%s=%d" % (b, buses[b]) for b in BUS_ORDER if b in buses))
    unknown = [r for r in rows if r["st"] == "?"]
    if unknown:
        print("bez statusu (%d):" % len(unknown))
        for r in unknown[:15]:
            print("   - %s" % r["n"])
    if check_only:
        return 0

    data = {
        "src": "docs/CZUJNIKI.md",
        "cats": sorted({r["c"] for r in rows}, key=lambda s: int(s.split(".")[0])),
        "rows": rows,
    }
    js = (
        "/* =============================================================================\n"
        " * Stacja Pogody - legenda czujnikow dla strony www\n"
        " * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985\n"
        " * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)\n"
        " * -----------------------------------------------------------------------------\n"
        " * PLIK GENEROWANY AUTOMATYCZNIE - nie edytuj recznie.\n"
        " * Zrodlo: docs/CZUJNIKI.md   Generator: tools/gen_legend.py\n"
        " * Po zmianie katalogu czujnikow uruchom: python tools/gen_legend.py\n"
        " * ========================================================================== */\n"
        "window.CZUJNIKI = "
        + json.dumps(data, ensure_ascii=False, separators=(",", ":"))
        + ";\n"
    )
    OUT.write_text(js, encoding="utf-8", newline="\r\n")
    print("zapisano %s (%d B)" % (OUT.relative_to(ROOT), len(js.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
