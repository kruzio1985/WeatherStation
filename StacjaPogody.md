# StacjaPogody — Pamięć sesji (do wznowienia w nowym czacie)

> Ten plik to pełna kopia wiedzy z sesji zakończonej **2026-10-11 ~04:50**.
> Aby kontynuować pracę w nowym czacie, powiedz agentowi:
> **„Przeczytaj `StacjaPogody.md` w głównym katalogu repo i kontynuuj od sekcji STAN OBECNY i DO ZROBIENIA."**

---

## 1. STAN OBECNY (najważniejsze — 2026-10-11 04:49)

| Urządzenie | IP | Stan |
|---|---|---|
| **Master** (ESP32-S3 WROOM-1 N16R8) | 192.168.1.143 | ❌ **NIE ODPOWIADA** (HTTP 000, brak ARP) — padł po wgraniu filesystemu. **Potrzebny power-cycle (odłącz zasilanie na ~10 s i podłącz).** Firmware z kodem c3_ działał wcześniej (C3 był odpytywany jako `outaq`, 14 odczytów ok). |
| **C3 air node** (ESP32-C3 SuperMini) | 192.168.1.147 | ✅ DZIAŁA — PM 7/8/9 µg/m³, T ~30 °C, RH ~66 %, p ~981 hPa, MQTT połączone, metryki pochodne publikowane. |
| Air station (zewnętrzna, wmbus) | 192.168.1.141 | ✅ DZIAŁA (prefix MQTT `stacja_powietrza`, user `wmbus`/`1234`). |
| Broker MQTT (HA) | 192.168.1.14:1883 | ✅ user `StacjaP` (hasło w pliku sekretów — nie drukować, nie commitować). |
| Sniffer VEVOR 868 MHz | 192.168.1.130 | Serwuje `/json`; **nie modyfikować**. |

**Sieć i router działają** (C3 i .141 odpowiadają HTTP 200). Problem dotyczy wyłącznie mastera.

---

## 2. CO TO JEST (projekt)

„Stacja Pogody" — firmware stacji meteorologicznej na **ESP32-S3 WROOM-1 N16R8 (master)**:
- Web UI (dashboard z panelami/wskaźnikami, kalibracja, konfiguracja, OTA, logi, kamera),
- MQTT → Home Assistant (prefix `stacja_pogody`),
- Logowanie CSV na kartę microSD 8 GB,
- Węzły podrzędne RS485, kamera, VEVOR 868 MHz (przez sniffer),
- Import czujników zewnętrznych (ExtDev): stacja powietrza .141 (`aq`), C3 (`outaq`), VEVOR.

Do tego nowy samodzielny węzeł **c3air** (ESP32-C3): AHT20 + BMP280 + PMS7003, WiFi + własna strona WWW + MQTT + OTA + metryki pochodne.

---

## 3. ŚCIEŻKI / REPOZYTORIA

- Główne repo (git): `C:\Projects\NOWY BACKUP\ESP\StacjaPogody\`
- Kopia robocza online (sync): `C:\Projects\OnlineWorkspace\StacjaPogody\`
- GitHub: `kruzio1985/WeatherStation`
- PlatformIO: `C:\Users\kruse\AppData\Local\Programs\Python\Python313\Scripts\pio.exe`
- Narzędzia esptool (penv): `C:\Users\kruse\.platformio\penv\Scripts\python.exe`
- Sesja (checkpointy): `c:\Users\kruse\.copilot\session-state\56ba0763-9ea4-4a10-bc7a-5eac5128c367\` (checkpoints/ + files/)

**Stan git:** HEAD lokalny `9055602` (c3air). GitHub HEAD `98e5aaa`.
Nieza-commitowane zmiany (praca nad sekcją „powietrze zewnątrz", master padł przed commitem):
`data/index.html`, `src/extdev.cpp`, `src/sensors.h`, `src/sensors_extra.cpp`.

---

## 4. BUDOWANIE I FLASHOWANIE (gotowe komendy)

### Master (S3)
```powershell
cd "C:\Projects\NOWY BACKUP\ESP\StacjaPogody"
# build
& "C:\Users\kruse\AppData\Local\Programs\Python\Python313\Scripts\pio.exe" run -e esp32s3-master
# OTA (firmware) — stacja musi być w sieci
& "...\pio.exe" run -e esp32s3-ota -t upload
# OTA (filesystem / strona WWW)
& "...\pio.exe" run -e esp32s3-ota -t uploadfs
```
> Uwaga: `esp32s3-ota` kończy się **exit code 1**, a w logu jest `SUCCESS` — to fałszywy alarm, ignorować.

### C3 air node (COM24, native USB VID 303A)
```powershell
cd "C:\Projects\NOWY BACKUP\ESP\StacjaPogody\c3air"
& "...\pio.exe" run -e esp32c3-air -t upload
# OTA: -e esp32c3-air-ota (192.168.1.147)
```

**Pełny flash C3 po zmianie tabeli partycji** (bootloader+partitions+app, zachowuje NVS):
```powershell
C:\Users\kruse\.platformio\penv\Scripts\python.exe C:\Users\kruse\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32c3 -p COM24 -b 460800 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```
(Systemowy Python nie ma zależności esptool — `intelhex` brakuje; używać penv.)

---

## 5. PINSY I CZUJNIKI

### C3 (c3air)
- I2C: SDA=8, SCL=9
- AHT20 (0x38) — temperatura + wilgotność
- BMP280 (0x77) — ciśnienie
- PMS7003: UART0 RX=20 / TX=21, 9600 (RS485 nieużywany w c3air)
- Konsola: USB-CDC (`Serial`), `Serial0`=UART0 PMS. **Brak Serial2 na C3.**

### Master (S3)
- `wd_heartbeat` — pin „życia" ~1 Hz (nowa rola pinu, domyślnie −1 = wyłączony).
- Pozostałe piny w `include/pins.h` + `src/pinmap.cpp`.

---

## 6. KLUCZOWE PUŁAPKI TECHNICZNE (gotchas — nie powtarzać błędów)

1. **PMS7003 (atmosferyczny, nie CF1):** ramka = buf po 2-bajtowym polu długości.
   `u16(0)=PM1.0 CF1`, `u16(2)=PM2.5 CF1`, `u16(4)=PM10 CF1`,
   **`u16(6)=PM1.0 atmosferyczny`, `u16(8)=PM2.5 atmosferyczny`, `u16(10)=PM10 atmosferyczny`**.
   Używać atmosferycznych (użytkownik wymagał). Po poprawce PM1 ≤ PM2.5 ≤ PM10.
2. **Adafruit BME280 lib odrzuca BMP280** (akceptuje tylko chip ID 0x60, BMP280 ma 0x58) → napisano surowy driver BMP280 (kompensacja Bosch: `bmp_tfine`, `dig_T1..P9` z 0x88–0x9E, `ctrl_meas 0xF4=0x27`).
3. **AHT20:** init 0xBE, status 0x71 bit3=kalibrowany, trigger 0xAC 0x33 0x00, odczyt 7 bajtów, `temp=raw*200/1048576-50`, `hum=raw*100/1048576`.
4. **`Wire.requestFrom(addr, (uint8_t)n)`** jest niejednoznaczne na tym rdzeniu — używać `int`.
5. **Konkatenacja String z ternariem `const char*`:** `"lit" + (warunek ? "a" : "b")` nie działa — opakować w `String(...)`.
6. **Task watchdog vs OTA:** `ArduinoOTA.handle()` blokuje pętlę; 15 s WDT odstrzeliwał przy ~95 % OTA. Fix: `esp_task_wdt_reset()` w `onProgress` (i w handlerze web Update). Na starcie `esp_ota_mark_app_valid_cancel_rollback()`.
7. **MQTT→HA wymaga pełnych nazw kluczy** (`state_topic`/`unique_id`/`device_class`), nie skrótów `stat_t`/`uniq_id`/`dev_cla`.
8. **extdev JSON bez BOM:** `Set-Content -Encoding UTF8` dodaje BOM → „Błędny JSON". Używać `[System.IO.File]::WriteAllText` z `UTF8Encoding($false)`.
9. **Daily restart:** okno minut **0–9** wybranej godziny (inaczej pętla restartów), min. 15 min uptime, ważny czas NTP (`time() >= 1e9`), **nigdy podczas OTA**, `config.saveState()` przed `ESP.restart()`.
10. **C3 `Serial`=USB CDC** (ARDUINO_USB_CDC_ON_BOOT=1).

---

## 7. CO ZROBIONO (historia)

Lokalny `git log` (od najnowszego):
- `9055602` **c3air** — samodzielny węzeł ESP32-C3 (AHT20+BMP280+PMS7003), WiFi+web+MQTT+OTA, metryki pochodne. (GitHub: `98e5aaa`)
- `16488cf` **Daily restart + heartbeat watchdog** (commit GitHub `3b8ddb9`).
- `ffc2e98` Fix watchdog reset na wykresach długiego dziennika (streaming `seriesFromLong`).
- `89a76ae` Docs: budżet watchdog dla wolnych operacji.
- `a4575e8` **ExtDev/VEVOR reliability** (backoff, timeouts, limit 256 B Tuya, filtr NaN).
- `479deaf` MQTT discovery: pełne nazwy kluczy + identyfikatory urządzeń.
- `c12a6bf` Fix fałszywego deszczu VEVOR ze złych ramek 868 MHz.
- Wcześniej (sesja sprzed): panele jakości powietrza `82efae0`, kamera `cf260a2`, WU/deszcz/gauges `e7ef5b8`/`4a9cd96`.

### Funkcje mastera (z tej sesji)
- **ExtDev/VEVOR:** backoff 60 s po 3 błędach / 300 s po 10, timeouts connect 2200 ms / read 1800 ms, budżet 4 s na wywołanie, `serviceTuya` czyta max 256 B, filtr `isfinite` przed ingestem/publikacją.
- **Daily restart** (opcjonalny): checkbox + godzina 00–23 na stronie Config, NVS (`extra.auto_restart`, `extra.auto_restart_hour`), warunki z pkt 9 powyżej. Endpoint `GET/POST /api/watchdog`.
- **Heartbeat ~1 Hz** na pinie `wd_heartbeat` (domyślnie −1) dla zewnętrznego modułu odcinającego zasilanie; działa też podczas OTA.
- **Sekcja „Stacja powietrza zewnątrz"** (c3_*) na pulpicie — kod zrobiony, **niezweryfikowany** (master padł przed weryfikacją).

### Funkcje c3air (węzeł C3)
WiFi static 192.168.1.147 + AP fallback `stacja-c3-XXXXXX`/`12345678`; web UI (dashboard z kolorami AQI/legendą/ostrzeżeniami, kalibracja, ustawienia WiFi/MQTT/geo/p0, OTA, diagnostyka, logi); MQTT→HA prefix `stacja_c3`; `/api/status` z `channels[]`; OTA dwubankowe z rollbackiem; log trwały LittleFS `/logs.txt` (flush co 5 min, trim ~12 KB); konfiguracja przez serial `wifi SSID PASS`.

### Metryki pochodne (c3air)
Punkt rosy (Magnus), wilgotność bezwzględna g/m³, heat index (T≥27 °C), wysokość barometryczna (p0 konfigurowalne), trend ciśnienia 3 h, średnie PM2.5 1/15/60 min, EPA AQI (max PM2.5/PM10) z kategorią+kolorami, stosunek PM2.5/PM10. Ostrzeżenia: RH>80 % (PM mniej wiarygodny), AQI>100, T<−10 °C. Geo: ręczne lat/lon lub pobierane z mastera (`/api/config` → latitude/longitude, co 15 min, tylko RAM).

---

## 8. WAŻNE PLIKI

Master (`src/`, `data/`, `include/`):
- `src/sensors_extra.cpp` — `OUTC3_IMPORT` (11 kanałów c3_*), `AIR_IMPORT`, rejestracja kanałów c3_* strefa „out", `ingestOutdoorAir()`, limit 256 B Tuya.
- `src/extdev.cpp` — typy `aq` (→ in_*) i `outaq` (→ c3_*), backoff/timeouts.
- `src/sensors.h` — deklaracja `ingestOutdoorAir`.
- `src/watchdog.cpp/.h` — daily restart + heartbeat.
- `src/api_json.cpp` — `/api/status` (autoRestart…, wdHeartbeat…).
- `src/web_server.cpp` — `/api/watchdog`.
- `data/index.html` — dashboard: filtr `isC3`, wykluczenie c3_ z list in/out, sekcja „Stacja powietrza zewnątrz".
- `include/pins.h`, `src/pinmap.cpp`, `src/board.cpp` — rola pinu `wd_heartbeat`.

C3 (`c3air/`):
- `c3air/src/main.cpp` — cały firmware (pojedynczy plik, ~25 KB).
- `c3air/platformio.ini`, `c3air/partitions.csv`.

---

## 9. DO ZROBIENIA (kolejność)

1. **Uratować mastera (.143):** power-cycle (odłącz zasilanie ~10 s). Po wstaniu sprawdzić `curl http://192.168.1.143/api/status`.
   - Jeśli wstanie → pkt 2–4.
   - Jeśli pętla bootowania → podejrzenia: (a) uszkodzony LittleFS po uploadfs → ponowny `uploadfs`; (b) crash nowego firmware — diagnoza tylko przez USB (COM9), jeśli OTA niemożliwy.
2. **Zweryfikować kanały c3_*** w `/api/status` mastera (powinny się pojawić, gdy master odpytuje C3 jako `outaq`).
3. **Zweryfikować sekcję „Stacja powietrza zewnątrz"** — `curl http://192.168.1.143/ | grep` (bez screenshotów!).
4. **Commit + push** nieza-commitowanych zmian (c3_ outdoor) do gita i GitHub; zsynchronizować z `C:\Projects\OnlineWorkspace\StacjaPogody\`; zweryfikować online build `esp32s3-master`.

---

## 10. PREFERENCJE UŻYTKOWNIKA (ważne!)

- Komunikacja **po polsku**, zwięźle — użytkownik jest niecierpliwy.
- **ŻADNYCH screenshotów / podglądów strony** — to najdroższe w tokenach. Weryfikacja tylko przez `curl` + `grep` / kod.
- Nie przeliczać bez potrzeby setek tysięcy linii kodu; pracować minimalnie i celnie.
- Nowy czat na nowy temat/projekt (oszczędność tokenów); ten plik + checkpointy umożliwiają wznowienie.
- Mastera **nie** ma pod USB — aktualizacje przez OTA.

## 11. SEKRETY

- Hasło MQTT (user `StacjaP`) i hasła WiFi są w pliku sekretów projektu — **nie drukować w czacie, nie commitować do gita**. Broker: 192.168.1.14:1883.
- WiFi mastera: SSID „SGC", static 192.168.1.143, brama .1.
