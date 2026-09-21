# Stacja Pogody — baza wiedzy o projekcie

> Ostatnia aktualizacja: **2026-09-21** · Stan firmware: **1.0.0 (master) / 1.0.0 (node)**
> Ten plik to skrót całej wiedzy o projekcie — hardware, architektura, komendy,
> stan prac i znane pułapki. Aktualizuj przy każdej większej zmianie.

---

## 1. Co to jest

Autonomiczna stacja pogodowa oparta o **ESP32-S3 WROOM-1 N16R8** (master), która:

- odpytuje po magistrali **RS485** zewnętrzne węzły z czujnikami (ESP32-C3 SuperMini),
- serwuje stronę **www** z plików **LittleFS** (podgląd, kalibracja, konfiguracja, wykresy),
- loguje dane do plików **CSV na karcie microSD** (8 GB),
- publikuje pomiary do **Home Assistant przez MQTT**,
- obsługuje **kamerę** (osobny moduł ESP32-CAM),
- udostępnia publiczną stronę z czujnikami + reklamą firmy (`public.html`).

Jeden kod źródłowy buduje wszystkie wersje firmware — rolę wybiera flaga
kompilacji `STACJA_ROLE_MASTER` (domyślnie 1 = master).

---

## 2. Sprzęt i piny

### Master — ESP32-S3 WROOM-1 N16R8
- Flash 16 MB, PSRAM 8 MB **octal** → `board_build.arduino.memory_type = qio_opi`
- Natywny port USB CDC (`ARDUINO_USB_CDC_ON_BOOT=1`), **COM9**, IP `192.168.1.143`
- Rola: **master, adres RS485 1**

| Funkcja | Pin | Uwagi |
|---|---|---|
| RS485 RX | GPIO 39 | do TX węzła (GPIO 21) |
| RS485 TX | GPIO 38 | do RX węzła (GPIO 20) |
| RS485 DE | GPIO 47 | kierunek nadawania |
| GPS TX/RX | GPIO 15 / 16 | NMEA, 9600 baud |
| SD | SD_MMC | /logs, /photos |

### Węzeł — ESP32-C3 SuperMini („c3mini")
- Flash 4 MB (XMC), bez PSRAM, **COM10**, MAC `1C:DB:D4:37:4D:7C`
- Rola: **węzeł, adres RS485 2**, działa wyłącznie po RS485 (bez Wi-Fi/www)
- RS485: RX GPIO 20, TX GPIO 21
- I²C: SDA GPIO 8, SCL GPIO 9 (m.in. AS5600, RTC)

### Kamera — ESP32-CAM AI-Thinker (osobny projekt `esp32cam/`)
- OV2640, flash DIO 40 MHz, 4 MB, partycje `huge_app.csv` (bez partycji OTA —
  OTA przez bibliotekę ArduinoOTA, port 3232, hasło `CAM_OTA_PASS` z `config.h`)
- Wysyła JPEG do mastera na `POST /api/camera/upload`; `/capture` robi zdjęcie
  i jednocześnie zwraca JPEG.

### Porty COM (stan na 2026-09-21)
| Port | Urządzenie | Rola |
|---|---|---|
| COM9 | ESP32-S3 | master |
| COM10 | ESP32-C3 | węzeł (slave) |
| COM11 | CH343 (inny projekt) | **nie dotykać** |

---

## 3. Architektura kodu

```
src/
  main.cpp            - pętla główna, start modułów
  config.h/.cpp       - ustawienia w NVS (wifi, mqtt, rs485_nodes, kalibracja, GPS)
  rs485.h/.cpp        - magistrala RS485: master ↔ węzły, protokół, lista węzłów
  sensors*.cpp        - obsługa czujników + zdalne kanały węzłów (x<adres>_...)
  web_server.cpp      - API + serwowanie www (LittleFS)
  camera.cpp          - kamera/timelapse/analiza obrazu
  mqtt_client.cpp     - publikacja do Home Assistant
  openmeteo.cpp       - prognoza 7 dni
  analysis.cpp        - rekordy, statystyki, róża wiatrów, AQI
  alerts.cpp          - progi i alerty
  weather_services.cpp- AWEKAS / WeatherCloud / CWOP + raporty
  ota.cpp             - ArduinoOTA (port 3232)
  pinmap.cpp          - mapowanie pinów per płytka
data/
  index.html          - główna strona www
  public.html         - strona publiczna (czujniki + reklama firmy)
  i18n.js             - tłumaczenia PL/EN/DE
esp32cam/             - firmware kamery (osobny projekt)
tools/                - build_release.ps1, skrypty pom.
docs/                 - TODO.md, CZUJNIKI.md, ten plik
dist/                 - gotowe binarki wydania
```

**Kluczowe endpointy API** (wszystkie GET, chyba że inaczej):
`/api/status`, `/api/config` (GET/POST), `/api/sensors`, `/api/gps`, `/api/astro`,
`/api/forecast`, `/api/forecast/analysis`, `/api/lightning`, `/api/sd/list`,
`/api/camera` + `/api/camera/upload` (POST), `/api/photos`, `/api/logs`,
`/api/windrose`, `/api/aqi`, `/api/analysis`, `/api/reports`, `/api/reports/export`,
`/api/alerts`, `/api/openmeteo`, `/api/syslog`, `/api/diagnostics`, `/api/backup`,
`/api/public`, `/api/wifi/scan`, `/api/pins`, `/api/boards`, `/api/ring`, `/api/rtc`,
`/api/services`, `/api/rs485`, `/api/rs485/action`, `/api/node`, `/api/reboot` (POST).

---

## 4. Budowanie i wgrywanie

### Kompilacja (bez sprzętu)
```powershell
cd C:\Projects\StacjaPogody
pio run -e esp32s3-master     # master
pio run -e esp32c3-node       # węzeł C3
pio run -e esp32s3-master -t buildfs   # obraz LittleFS
```

### Wgrywanie mastera przez OTA (urządzenie w sieci)
```powershell
pio run -e esp32s3-ota -t upload --upload-port 192.168.1.143
pio run -e esp32s3-ota -t uploadfs --upload-port 192.168.1.143   # pliki www
```
Hasło OTA: `stacja-ota` (ustawiane na www → „Aktualizacja"). NVS **nie** jest kasowane.

### Wgrywanie węzła po USB
```powershell
pio run -e esp32c3-node -t upload --upload-port COM10
```

### Środowiska
`esp32s3-master`, `esp32s3-node` (+ `-8mb`, `-8mb-quad`), `esp32c3-node`
(+ `-addr3..8`, `-v3`), `esp32c6-node` (+ `-addr3..8`), `esp32s3-ota`.

### Wydanie
```powershell
powershell -File tools\build_release.ps1   # buduje wszystkie bin + kamera → dist/
```

---

## 5. Funkcje (zrealizowane)

1. Pulpit + wykresy dzienne/tygodniowe/miesięczne + porównawcze.
2. Rekordy + statystyki dzienne + trend ciśnienia.
3. Alerty/progi → MQTT/HA.
4. Prognoza Open-Meteo (7 dni) + analiza prognozy.
5. Róża wiatrów + wykresy porównawcze.
6. AQI + analiza PM2.5/PM10.
7. Kamera / timelapse + lekka analiza obrazu (jasność/luks/dzień-noc/RGB/ruch/chmury).
8. Raporty + retencja plików SD + serwisy AWEKAS/WeatherCloud/CWOP.
9. Wiele węzłów RS485: lista w NVS (`rs485_nodes`), dodawanie/usuwanie, zmiana
   adresu węzła po magistrali (`assignAddr` + komenda `addr`), round-robin,
   zdalne kanały `x<adres>_<id>` (u węzła 2 → `x2_...`), stronicowanie `/api/node?c=chan`.
10. Strona www PL/EN/DE (i18n.js), kalibracja czujników, konfiguracja pinów.
11. Logi CSV na SD (`/logs/YYYY-MM.csv`), podgląd/pobieranie/formatowanie karty,
    eksport plików — z poziomu www.
12. GPS (NMEA) + ręczne współrzędne do Słońca/Księżyca/prognozy lokalnej.
13. Publiczna strona czujników z reklamą firmy (`/public.html`).

---

## 6. Stan prac (2026-09-21)

- **Wgrany i zweryfikowany na sprzęcie:** master 1.0.0 (OTA), węzeł C3 1.0.0 (USB),
  LittleFS (OTA). RS485 online, **179 kanałów** zmirrorowanych, logi SD OK.
- **Todo:** 15/16 done. Zablokowane: `cam-analysis` — kod gotowy, brak fizycznego
  ESP32-CAM do testu end-to-end.
- **i18n PL/EN/DE — pokrycie ukończone (2026-09-21):** audyt `t()` + audyt
  literałów JS (diakrytyki/znane słowa) wyczyszczony do samych false-positivów
  (akronimy: ALARM/GPS/PM2.5/PSRAM…; fragmenty obsługiwane przez `REGEX`;
  komentarze; artefakty `innerHTML`). Ostatnie uzupełnienia: kamera-piny
  (`XCLK (zegar)`, `PCLK (zegar pikseli)`, `(wymagany)`, `(opcjonalny)`),
  podpowiedzi serwisów `AWEKAS`/`CWOP`/`Weathercloud`, `Analiza`, `Brak`,
  `Długość` (=GPS), `Noc`, `Ruch`, `spada - możliwa zmiana pogody`,
  `Dane kompletne (przeskanowano archiwum CSV)` i in. Zweryfikowane na żywo
  (EN + DE) na stronie Kamera i Usługi.
- **Znane drobiazgi:**
  - GPS ręczny: pola przyjmują **liczby** (szer./dł.), nie adres tekstowy —
    wpisanie „Kraków Mała Góra 18" nic nie daje, bo to nie geokoder. Wpisz
    współrzędne liczbowe. Backend zapisuje i od razu przelicza astro/prognozę
    (zweryfikowane: POST `/api/config` → `/api/astro` aktualizuje się natychmiast).

---

## 7. Pułapki i dobre rady

- **PSRAM octal** — na N16R8 musi być `qio_opi`; na modułach N8R2/N8R4 (quad) → `qio_qspi`.
- **Kamera: wysyłanie binarnego JPEG** — nie istnieje przeładowanie
  `send(200, type, buf, len)`. Poprawnie:
  `setContentLength(len); send(200, "image/jpeg", ""); sendContent((const char*)buf, len);`
- **camera_pins.h** trzeba dołączyć jawnie po `#define CAMERA_MODEL_AI_THINKER` —
  biblioteka esp32-camera sama go nie dołącza.
- **Flash 16 MB** — rozmiar bierze się z `board_upload.flash_size` (nie `board_build.flash_size`).
- **Konsola mastera** — bez `ARDUINO_USB_CDC_ON_BOOT=1` na COM9 nic nie widać
  (Serial to UART0 GPIO43/44, a nie port USB).
- **Nie kasuj NVS** przy wgrywaniu (ani `erase_flash`) — utracisz Wi-Fi/MQTT/kalibrację/listę węzłów.
- **esptool v5** — flagi `--flash_mode/--flash_freq/--flash_size/merge_bin` są
  przestarzałe; nowe to `--flash-mode/--flash-freq/--flash-size/merge-bin`.
- **Kompilacja węzła** wyklucza `web_server.cpp`, `mqtt_client.cpp`, `weather_services.cpp`,
  `ota.cpp`, `alerts.cpp`, `openmeteo.cpp` (patrz `build_src_filter`).
- **PowerShell (Windows)** — brak `&&`/`||`; łączyć przez `;` i `if ($?)`.
  Używać `curl.exe` (nie `Invoke-WebRequest`).
- **Seriale w tym środowisku** — po kompilacji `Serial` mastera to USB-CDC;
  zapis do Serial przy odpiętym hoście USB jest bezpieczny (core odrzuca bufor).

---

## 8. Hasła i poświadczenia

- Wszystkie sekrety (Wi-Fi, hasło OTA, klucze serwisów) żyją **tylko w NVS urządzenia**,
  nie w kodzie. Budowanie kopii zapasowej wyklucza `.pio/` (tam platforma trzyma
  `cfg.json` z hasłem Wi-Fi) — **nie komituj/nie udostępniaj `.pio\cfg.json`**.
- OTA mastera: `stacja-ota`. Kamera: `CAM_OTA_PASS` w `esp32cam/src/config.h`.

---

## 9. Kopie zapasowe

Kopia całego kodu (bez `.pio/`) jest w `C:\Projects\backups\StacjaPogody-<znacznik>.zip`.
Tworzenie:
```powershell
tar.exe -a -c -f "C:\Projects\backups\StacjaPogody-$(Get-Date -Format yyyyMMdd-HHmmss).zip" --exclude="./.pio" -C "C:\Projects\StacjaPogody" .
```
