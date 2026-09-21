#!/usr/bin/env python3
# =============================================================================
#  Stacja Pogody - synchronizacja statusow w katalogu czujnikow
#  Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
#  Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
# =============================================================================
#  Ustawia ✅ w docs/CZUJNIKI.md dla wierszy, ktore sa juz naprawde obslugiwane
#  przez firmware.  Wiersz dostaje ✅ tylko wtedy, gdy spelnione sa OBA warunki:
#
#    1. WSZYSTKIE kanaly z ostatniej kolumny wiersza istnieja w kodzie
#       (zebrane z: src/sensors.cpp .id="...", src/sensors_extra.cpp mkChan("...")
#        oraz tablic ChanDef w src/drv_*.cpp),
#    2. nazwa/chip z kolumny "Czujnik" wystepuje w kodzie drivera, ktory
#       faktycznie obsluguje dany kanal.
#
#  Warunek 2 jest po to, zeby sam fakt istnienia np. kanalu `temp` nie
#  oznaczal automatycznie PT100, termopary czy NTC - musza byc wymienione
#  w kodzie drivera.
#
#  Uruchomienie (z katalogu projektu):
#      python tools/sync_status.py            # pokaz kandydatow, nie zapisuj
#      python tools/sync_status.py --write     # zapisz zmiany w docs/CZUJNIKI.md
#
#  Po zapisie uruchom: python tools/gen_legend.py   (i wgraj LittleFS)
# =============================================================================
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "CZUJNIKI.md"

STATUS_OK = "\u2705"
STATUS_PLAN = "\U0001f7e1"
STATUS_NO = "\u26d4"

# Pliki zrodlowe, w ktorych szukamy kanalow i nazw ukladow.
ID_FILES = ["src/sensors.cpp", "src/sensors_extra.cpp"]
DRV_GLOB = "src/drv_*.cpp"

# Moduly firmware spoza drv_*, ktore realnie obsluguja sprzet (szukamy w nich
# tylko nazw ukladow - warunek 1 nadal sprawdza kanaly w drv_*/sensors*).
EXTRA_MODEL_FILES = [
    "src/gps.cpp",          # NEO-6M/7M/8M, ATGM336H, L76K
    "src/lightning.cpp",    # AS3935
    "src/astro.cpp",        # pozycja Slonca, wschod/zachod
    "src/led_ring.cpp",     # WS2812B
    "src/sd_card.cpp",      # karta microSD
]

# Recznie potwierdzone wiersze (nazwa wiersza -> wymagane kanaly), gdy ostatnia
# kolumna nie zawiera identyfikatora kanalu (np. "zdarzenie w logu").
# Pusta lista = funkcja gotowa, ale firmware nie wystawia jej jako kanalu
# (np. wschod/zachod Slonca liczy src/astro.cpp na potrzeby pulpitu i LED).
MANUAL = {
    "MCP23017": ["mcp_in*"],
    "PCF8574 / PCF8575": ["mcp_in*"],
    "Wschód / zachód / górowanie": [],
    # radar: kanal powstanie razem z driverem LD2410/LD2450 (dotad 🟡)
    "HLK-LD2410": ["radar_rx"],
    "HLK-LD2450": ["radar_rx"],
    "HLK-LD2461": ["radar_rx"],
}

# Tokeny, ktore wygladaja jak model, ale modelem nie sa.
NOT_A_MODEL = {
    "ADC1", "ADC2", "ADC", "I2C", "I2S", "SPI", "UART", "GPIO", "RS485", "USB",
    "ESP32", "ESP32S3", "N16R8", "N8R4", "CRC8", "CRC16", "CSV", "MQTT", "HA",
    "NVS", "OTA", "SD", "RTC", "LED", "PWM", "JSON", "HTML", "HTTP", "TCP",
    "V3", "V3V3", "V12", "V5", "V24", "MS5611_PROM",
}

# Rozne nazwy handlowe tego samego ukladu (wiersz katalogu -> nazwa w kodzie).
ALIAS = {
    "AM2301": "AM2302",
    "AM2321": "AM2320",
    "SHT21": "SI7021",
}

MODEL_RE = re.compile(r"\b[A-Za-z][A-Za-z]{0,3}[-_ ]?[A-Za-z]?\d[A-Za-z0-9]*\b")
ID_DEF_RE = re.compile(r'\.id\s*=\s*"([^"]+)"')
MKCHAN_RE = re.compile(r'mkChan\(\s*"([^"]+)"')
CHAN_ROW_RE = re.compile(r'^\s*\{\s*"([a-z0-9_]+)"\s*,')
BACKTICK_RE = re.compile(r"`([^`]+)`")
TOKEN_OK_RE = re.compile(r"^[a-z0-9_]+(\*)?$")

# Identyfikatory danych udostepnianych poza SensorManager (bez wlasnego kanalu),
# ale realnie dzialajacych - warunek 1 uznaje je za spelnione.
EXTRA_IDS = {
    "gps",      # zakladka GPS + MQTT: pozycja, czas, satelity (src/gps.cpp)
}


def norm_model(text: str) -> str:
    """Normalizuje nazwe ukladu: MQ-7 -> MQ7, Si7021 -> SI7021."""
    return re.sub(r"[^A-Z0-9]", "", text.upper())


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def collect_ids():
    """Zbior zaimplementowanych identyfikatorow kanalow."""
    ids = set()
    for rel in ID_FILES:
        p = ROOT / rel
        if not p.exists():
            continue
        t = read(p)
        ids.update(ID_DEF_RE.findall(t))
        ids.update(MKCHAN_RE.findall(t))
        if re.search(r'"ds_"\s*\+', t) or '"ds_"' in t:
            ids.add("ds_*")                     # kazdy DS18B20: ds_0..ds_7
    for f in sorted(ROOT.glob(DRV_GLOB)):
        if f.name in ("drv_mod.cpp", "drv_i2c.cpp", "drv_table.cpp"):
            continue
        for line in read(f).splitlines():
            m = CHAN_ROW_RE.match(line)
            if m:
                ids.add(m.group(1))
    ids.update(EXTRA_IDS)
    return ids


NOT_SUPPORTED_MARKERS = (
    "todo", "planowan", "do zrobienia", "nie obslug", "nie obs\u0142ug",
    "brak wsparcia", "niewspieran", "przyszl", "przysz\u0142", "docelowo",
)


def collect_models():
    """Uklady wymienione w kodzie obslugujacym czujniki (bez linii 'planowane')."""
    files = []
    for f in sorted(ROOT.glob(DRV_GLOB)):
        if f.name in ("drv_mod.cpp", "drv_table.cpp"):
            continue
        files.append(f)
    for rel in ID_FILES:
        p = ROOT / rel
        if p.exists():
            files.append(p)
    for rel in EXTRA_MODEL_FILES:
        p = ROOT / rel
        if p.exists():
            files.append(p)
    where = {}
    for f in files:
        for line in read(f).splitlines():
            low = line.lower()
            if any(m in low for m in NOT_SUPPORTED_MARKERS):
                continue
            for m in MODEL_RE.finditer(line):
                n = norm_model(m.group(0))
                if len(n) < 4 or n in NOT_A_MODEL:
                    continue
                if not any(c.isdigit() for c in n):
                    continue
                where.setdefault(n, f.name)
    return where


def ids_from_cell(cell: str):
    """Identyfikatory kanalow z ostatniej kolumny wiersza."""
    out = []
    for raw in BACKTICK_RE.findall(cell):
        tok = raw.strip()
        if " " in tok or ":" in raw:
            continue                            # np. `SHT4x: ` to nie kanal
        if not TOKEN_OK_RE.match(tok):
            continue
        out.append(tok)
    return out


def id_done(channel: str, ids: set) -> bool:
    if channel in ids:
        return True
    if channel.endswith("*"):
        pref = channel[:-1]
        return any(i.startswith(pref) for i in ids)
    return False


def cell_tokens(cell: str):
    """Tokeny-model z kolumny 'Czujnik' (z uwzglednieniem aliasow)."""
    txt = re.sub(r"[*`|]", " ", cell)
    out = set()
    for m in MODEL_RE.finditer(txt):
        n = norm_model(m.group(0))
        if len(n) < 4:                          # krotkie tokeny (S8, M8Q) sa zbyt niejednoznaczne
            continue
        out.add(ALIAS.get(n, n))
    return out


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    write = "--write" in sys.argv
    if not DOC.exists():
        print("brak pliku: %s" % DOC)
        return 1

    ids = collect_ids()
    models = collect_models()
    print("kanaly w firmware: %d   ukladow wymienionych w kodzie: %d" % (len(ids), len(models)))

    lines = DOC.read_text(encoding="utf-8").splitlines()
    flipped = []
    by_id_only = []
    no_id = []

    for idx, line in enumerate(lines):
        if not line.startswith("|") or STATUS_PLAN not in line:
            continue
        # status moze wystapic wielokrotnie (np. "BME680 ✅🟡") - wtedy wiersz
        # juz byl czesciowo potwierdzony, ale i tak sprawdzamy warunki
        if STATUS_OK in line:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue
        name = re.sub(r"[*`]", "", cells[0])
        name = name.replace(STATUS_PLAN, "").replace(STATUS_OK, "").strip()
        if not name:
            continue
        cell_ids = ids_from_cell(cells[-1])
        ok_channels = [c for c in cell_ids if id_done(c, ids)]
        missing = [c for c in cell_ids if not id_done(c, ids)]
        if not cell_ids:
            man = next((v for k, v in MANUAL.items() if name.startswith(k)), None)
            if man is not None and all(id_done(c, ids) for c in man):
                lines[idx] = line.replace(STATUS_PLAN, STATUS_OK, 1)
                why = "recznie: " + (",".join(man) if man else "funkcja w kodzie, bez kanalu")
                flipped.append((name, why))
            else:
                no_id.append(name)
            continue
        # warunek 1: wszystkie kanaly istnieja
        if missing:
            continue
        # warunek 2: WSZYSTKIE uklady z nazwy wiersza wystepuja w kodzie firmware
        toks = cell_tokens(cells[0])
        hit = None
        partial = []
        for t in toks:
            if t in models:
                if hit is None:
                    hit = "%s (w %s)" % (t, models[t])
            else:
                partial.append(t)
        if partial:
            by_id_only.append("%s   [brak: %s]" % (name, ", ".join(sorted(partial)[:4])))
            continue
        if hit is None:
            by_id_only.append(name)
            continue
        lines[idx] = line.replace(STATUS_PLAN, STATUS_OK, 1)
        flipped.append((name, hit))

    print("\n=== do oznaczenia jako ✅ (%d) ===" % len(flipped))
    for n, why in flipped:
        print("  + %-52s %s" % (n[:52], why))
    print("\n=== kanaly gotowe, ale ukladu nie ma w kodzie (%d) - zostaja 🟡 ===" % len(by_id_only))
    for n in by_id_only:
        print("  - %s" % n[:80])
    print("\n=== brak identyfikatora kanalu w wierszu (%d) - bez zmian ===" % len(no_id))
    for n in no_id:
        print("  ? %s" % n[:80])

    if not flipped:
        print("\nnic do zmiany")
        return 0
    if not write:
        print("\n(wersja testowa - dodaj --write, aby zapisac)")
        return 0
    text = "\r\n".join(lines) + "\r\n"
    DOC.write_text(text, encoding="utf-8", newline="")
    print("\nzapisano docs/CZUJNIKI.md (%d wierszy oznaczonych)" % len(flipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
