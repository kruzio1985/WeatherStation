# Stacja Pogody — zasady pracy (oszczędzanie tokenów)

> Te zasady obowiązują **każdy czat** pracujący w tym projekcie. Zostały wpisane
> na życzenie właściciela projektu. Nie usuwaj ich bez jego zgody.

## ZAKAZ — operacje drogie (zjadają tokeny)

1. **Podagenty / delegowanie** — `task`, dowolny `agent_type` (`explore`,
   `general-purpose`, `research`, `code-review`, `security-review`).
   Każde uruchomienie to osobny, pełny kontekst. **Zakaz.**
2. **Czytanie całych dużych plików** — `view` bez `view_range` dla plików > 20 kB:
   - `data/index.html` (~380 kB)
   - `data/i18n.js` (~205 kB)
   - `data/legend.js` (~56 kB)
   - `c3air/src/main.cpp` (~76 kB)
   - `src/drv_display.cpp`, `src/web_server.cpp`, `src/api_json.cpp`
   - `StacjaPogody.md`, `docs/PROJEKT-WIEDZA.md`
   - `OfflineWorkspace\HISTORIA\StacjaPogody\BLEDY-I-NAPRAWY.md` (50 kB), `TODO.md` (28 kB)
   Używać **wyłącznie** `view_range=[start, end]` albo `grep` z `-n` i `head_limit`.
3. **Zrzuty surowych odpowiedzi HTTP z urządzeń** — `curl` bez filtra. Szczególnie:
   - `/api/status` mastera (~51 kB), `/api/sensors` (~43 kB)
   - strony HTML węzłów (każda odpowiedź ma ~2 kB inline CSS)
   Zawsze filtrować: `Select-String`, `ConvertFrom-Json`, `python -c` z wybranymi polami.
4. **Logi bez limitu** — `/log.txt`, `/api/logs`. Filtrować `Select-String` +
   `Select-Object -First N`.
5. **Screenshoty i podgląd obrazów** — `screenshotPage`, `view` na `.jpg`/`.png`.
   Razem z narzędziami przeglądarki (`openBrowserPage`, `readPage`,
   `runPlaywrightCode`) — bardzo drogie. **Zakaz**, chyba że użytkownik wprost poprosi.
6. **`grep` bez `head_limit`** na dużych plikach, zwłaszcza z `-C`/`-A`/`-B`.
   Wynik może mieć setki linii.
7. **Powtarzanie pomiarów i weryfikacji** — pętle `for` z `curl`, wielokrotne
   sprawdzanie tego samego. **Jedna weryfikacja na koniec zadania.**
8. **`view` z `forceReadLargeFiles`** — wciąga cały plik do kontekstu.
9. **Nieocenzurowane diffy** — `git diff` całego pliku. Używać `--stat` i wskazanych
   fragmentów (`git diff -- <plik> | Select-Object -Skip N -First M`).
10. **Wielokrotne budowanie** — buildować raz; powtarzać tylko na wyraźne żądanie
    (projekt wymaga 2× SUCCESS przed wgraniem, ale bez dodatkowych przebiegów).
11. **Szukanie kluczy w `i18n.js`/`legend.js` przez czytanie pliku** — używać
    `grep` z `head_limit`.

## Zasady ogólne

- Odpowiedzi **krótkie i konkretne** — bez zbędnych opisów i podsumowań.
- Nie uruchamiać diagnostyki „na wszelki wypadek" — tylko to, o co prosi użytkownik.
- Komunikacja po polsku.

## Zakazy sprzętowe (stałe, niezmienne)

- **Nie ruszać Wi-Fi laptopa** użytkownika.
- **Nie kasować/uszkadzać NVS** ani partycji danych.
- **Nie używać** `POST /api/factoryreset` ani `POST /api/sd/format` bez wyraźnej zgody.
- **Nie podłączać** stacji przez USB — aktualizacja wyłącznie OTA.
- Zmieniać konfigurację **tylko** przez `POST /api/config`, `POST /api/display`,
  `POST /api/pins`.

## Środowisko (skrót)

- Repo robocze: `C:\Projects\NOWY BACKUP\ESP\StacjaPogody`
- Master: `192.168.1.143`. Węzeł powietrza zewnętrznego (.147): **klasyczny ESP32**,
  4 MB flash → target `esp32-air-4mb` (NIE `esp32c3-air`!). Slot app `0x1A0000`.
- Węzeł powietrza wewnętrznego: `192.168.1.141`.
- Kamera: `192.168.1.142`.
- Publikowanie: `POST /api/ota/firmware` i `POST /api/ota/filesystem` z nagłówkiem
  `X-OTA-Key`, albo `POST /update` (węzły, pole `firmware`).
