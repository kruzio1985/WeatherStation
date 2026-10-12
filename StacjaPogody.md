# StacjaPogody — Pamięć sesji (do wznowienia w nowym czacie)

> Ten plik to pełna kopia wiedzy z sesji zakończonej **2026-10-11 ~06:25**.
> Aby kontynuować pracę w nowym czacie, powiedz agentowi:
> **„Przeczytaj `StacjaPogody.md` w głównym katalogu repo i kontynuuj od sekcji STAN OBECNY i DO ZROBIENIA."**

---

## 1. STAN OBECNY (najważniejsze — 2026-10-11 06:25)

| Urządzenie | IP | Stan |
|---|---|---|
| **Master** (ESP32-S3 WROOM-1 N16R8) | **192.168.1.143 (statyczne)** | ✅ **DZIAŁA** — `/` → 200, `/api/status` → 200. MAC `80:b5:4e:c6:52:a8`. Podłączony kablem USB do PC (COM9, USB-Serial/JTAG). |
| **C3 air node** (ESP32-C3 SuperMini) | 192.168.1.147 | ✅ DZIAŁA — master odpytuje go jako ExtDev (`outaq`) → kanały `c3_*` w `/api/status`. |
| Air station (zewnętrzna, wmbus) | 192.168.1.141 | ✅ DZIAŁA — master odpytuje ją jako ExtDev (`aq`) → kanały `in_*`. |
| Broker MQTT (HA) | 192.168.1.14:1883 | ✅ user `StacjaP` (hasło w pliku sekretów — nie drukować, nie commitować). |
| Sniffer VEVOR 868 MHz | 192.168.1.130 | ✅ Serwuje `/json`; **nie modyfikować**. |

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

**Stan git:** lokal `main` — commit z **11.10 23:50** (bez pusha), GitHub `98e5aaa` — **nic nie wypchnięte** (życzenie użytkownika
z 11.10 06:16: „nic na githuba niewgrywaj"). Lokal **ahead 20 / behind 8**, ale **treść identyczna**
(`git diff 9055602 98e5aaa` = pusto — GitHub ma squashowaną historię), więc push wymaga
`--force-with-lease` albo merge.

---

## AKTUALIZACJA (11.10, 23:50) — MQTT + porządki

- **MQTT mastera: było WYŁĄCZONE**, jest **WŁĄCZONE**. `POST /api/mqtt/test` (przyjmuje ustawienia
  w treści, robi `applyJson` + `reconnectWithConfig`) z danymi z `SEKRETY\mqtt pogoda.txt`:
  `ok:true`, „Połączono z 192.168.1.14:1883 (27 ms)", prefix `stacja_pogody`, user `StacjaP`.
  **Bez restartu i bez flashowania.** Weryfikacja u brokera (paho z PC): **41 stanów** + **41 discovery**.
- **Sekcja „Stacja powietrza zewnątrz": ZWERYFIKOWANA** — serwowany `index.html` = lokalny co do bajtu
  (349 251 B, MD5 `FA6BE41110B499FB66F83D7F4AA6DA09`), 11 kanałów `c3_*` z wartościami.
- **Build `esp32s3-master`: 2× SUCCESS** (RAM 26,7 %, Flash 56,5 %, 0 ostrzeżeń, 0 błędów).
- **Backup:** `C:\Projects\NOWY BACKUP\ESP\StacjaPogody.zip` — 44,6 MB, **bez `.pio`**, **z `.git`**,
  jeden plik (nadpisywany). Uwaga na przyszłość: `Compress-Archive` **gubi pliki ukryte** (czyli `.git`)
  i zapisuje backslashe w nazwach — używać `tar.exe -a -c -f …` (bsdtar).
- **Git: nadal nic nie wypchnięte** (życzenie użytkownika). Zmiany **zacommitowane lokalnie**
  (11.10 23:50): „Stacja: import powietrza zewnętrznego z C3 (c3_*) + fix Wi-Fi przy pustym SSID".

---

## AKTUALIZACJA (12.10, 07:30) — ekran TFT, diagnostyka I2C, czujnik na kanale 7

- **Ekran GMT024-10 / ST7789 (osobna magistrala SPI): DZIAŁA.** Sterownik `src/drv_display.{h,cpp}`
  (LovyanGFX, bufor klatki w PSRAM, układ poziomy 320×240 przy `rotate=1`), piny `SCK=40, MOSI(SDA)=7,
  CS=21, DC=41, RST=3`, zakładka „Wyświetlacz TFT" + `GET/POST /api/display`. Pełny redraw 77–78 ms
  (bez bufora 118 ms). Uwaga: na module „SDA" = SPI **MOSI**, nie I2C — nie wpinać w rozdzielacz.
- **Diagnostyka magistrali I2C:** `GET /api/i2c` (mapa per kanał PCA9548A + stan linii SDA/SCL),
  `POST /api/i2c/scan` (skan i wykrycie czujników T/H/P **bez restartu**) oraz
  `POST /api/i2c/probe` (surowe bajty układu 0x38 → pole `probe038` w `/api/i2c`).
  Poprawka: `addrList()` zwracał adresy **bez cudzysłowów** (`[0x57,0x68]`) → `/api/i2c` był
  niepoprawnym JSON-em; teraz `["0x57","0x68"]`.
- **Kanał 7 rozdzielacza, adres 0x38 — DZIAŁA.** To **nie jest SHT3x** (Sensirion ma sztywno
  0x44/0x45), a klon **AHT2x** w obudowie opisanej „SHT3X": CRC Sensiriona nie pasuje, odpowiedź ma
  układ *status + 5 bajtów* (`18` + RH[19:0] + T[19:0]). Odczyt: **27,3 °C / 28,0 %** →
  `ths_aht_t`/`ths_aht_h` → MQTT `stacja_pogody/sensor/ths_aht_*`. Nazwy encji: **AHT30** (użytkownik
  zidentyfikował moduł; ten sam sterownik obsługuje całą rodzinę AHT10/20/21/25/30/31 — identyczny
  protokół, różni się tylko dokładność i zakres temperatury).
- **Naprawione w `drv_ths.cpp`:** komenda pomiaru AHT szła jako **4 bajty** (`00 AC 33 00`) zamiast
  3 (`AC 33 00`) — dodany `i2cuWrite3()` w `i2c_util.h`; `ahtRead()` **kasowało wynik po 1 s**
  (pętla czyta rzadziej) → kanały zostawały wiecznie puste — teraz czeka na bit „busy" w bajcie
  statusu; `ahtProbe()` najpierw inicjalizuje układ (0xE1 / 0xBE), dopiero potem wymaga bitu CAL.
- Mapa magistrali (07:30): ch0 `0x57,0x68`, ch1 `0x23`, ch2 —, ch3 `0x76` (BMP280), ch4 —, ch5 —,
  ch6 —, ch7 `0x38` (AHT2x). Multiplekser `0x70`, razem 5 układów.
- **Nadal nie wgrane:** `uploadfs` (strona z zakładką „Wyświetlacz TFT" i mapą I2C — serwowany
  `index.html` jest starszy niż lokalny).

---

## AKTUALIZACJA (12.10, 09:50) — AWARIA: stacja zamarzła na ExtDev + punkt rosy + polskie znaki

### 1. AWARIA (najważniejsze): pętla główna zawieszona przez martwy węzeł C3
- **Objaw:** „dziwne odczyty" — wszystkie kanały zamrożone (UV 31,84 · światło 20474 · temp 27,18),
  `draws` ekranu stał w miejscu, OTA przestało odpowiadać (`No response from the ESP`, 3 próby),
  a `POST /api/reboot` zwracało `{"ok":true}` i **nic się nie działo**.
- **Przyczyna:** log kończył się na `ExtDev: poll http://192.168.1.147/api/status` (węzeł C3).
  C3 odpowiadał na ping (466/109 ms), ale **nie na HTTP** — a `HTTPClient` potrafi zablokować
  wątek bez końca, mimo ustawionych limitów (2,2 s connect / 1,8 s read). Pętla główna czekała
  w nieskończoność → `ESP.restart()` z `/api/reboot` (deferred do pętli, `main.cpp:535`) nigdy nie
  został wykonany. Wcześniej ten sam błąd dawał „obieg trwał 9821 ms".
- **Fix (`extdev.cpp`):** własne gniazdo `WiFiClient` zamiast `HTTPClient`, **bez ani jednego
  blokującego oczekiwania** — czytamy tylko to, co już przyszło (`available()`), z twardym budżetem
  (connect 1,2 s + całość 1,8 s) i zamknięciem gniazda po przekroczeniu. Backoff wydłużony:
  3 błędy → 1 min, 6 → 5 min, **10 → 30 min** (było 5 min). Log pokazuje czas każdego odczytu.
- **Dowód po naprawie:** przy martwym C3 `brak odpowiedzi (1838 ms)` i pętla szła dalej;
  po restarcie C3: VEVOR **97 ms**, stacja powietrza **312 ms**, C3 **721 ms**, pętla żyje.

### 2. Punkt rosy (porównywanie wilgotności różnych czujników)
- Nowe kanały: **`ths_aht_dp`** (AHT30, z jego własnych T/RH) i **`in_air_dp`** (stacja powietrza .141).
- Wspólny wzór Magnusa: `dewPointMagnusC()` w `sensors.h`. Wilgotność % zależy od temperatury
  czujnika, więc porównuje się **punkt rosy** — on zależy tylko od ilości wody w powietrzu.
- Zmierzone 12.10 ~09:45: AHT30 `26,86 °C / 26,88 %` → punkt rosy **6,21 °C**;
  stacja powietrza `25,41 °C / 46,29 %` → punkt rosy **13,03 °C**. Różnica 6,8 °C punktu rosy
  w tym samym pomieszczeniu to **nie** błąd zaokrągleń i nie dokładność czujnika (±3 % RH = 3 pkt proc.).
- Do MQTT idą **tylko kanały strefy „out"** — `in_air_dp` jest widoczny na pulpicie, ale nie w HA
  (zamierzone: dane wewnętrzne publikuje sama stacja powietrza).

### 3. Polskie znaki na wyświetlaczu TFT
- **Czcionki wbudowane nie mają polskich liter:** `DejaVu18` z paczki LovyanGFX ma zakres tylko
  `0x20–0x7E`, a IPA/efont to pliki źródłowe po 25–33 MB. Dlatego `drv_display.cpp` **rysuje znaki
  diakrytyczne sam**: litera bazowa + kreska (ć ń ó ś ź), kropka (ż), ogonek (ą ę), kreska przez
  literę (ł); obsłużone też `°`, `µ`, `²`, `³`. Wszystkie napisy idą przez `plDraw()`
  (z wyrównaniem jak `drawString`).
- **Test bez patrzenia na ekran:** `GET /api/pltest` renderuje napis do bufora i liczy piksele —
  wynik: `plain=713 → accent=725`, `plain2=1145 → accent2=1162`, `ok:true` (diakrytyki się rysują).

### 4. PUŁAPKA: router AsyncWebServer dopasowuje po PREFIKSIE
- `GET /api/display/pltest` **przechwytywał handler `/api/display`**, a `POST /api/extdev/remove`
  trafiał do `POST /api/extdev` (dlatego usuwanie urządzenia zawsze zwracało `{"ok":false}`).
- **Zasada na przyszłość:** nie zagnieżdżać nowych ścieżek pod istniejącymi trasami.
  Zmienione: `/api/display/pltest` → **`/api/pltest`**, `/api/extdev/remove` → **`/api/extdev-del`**
  (plus `data/index.html`).

### 5. UV = 31,8 → to NIE błąd przeliczenia
- `uv` = analogowy GUVA-S12SD na `uv_adc` (GPIO4), `UVI = mV / 100`. Odczyt 31,84 = **3184 mV**,
  czyli pin na sufcie ADC. Sprawny GUVA-S12SD daje maks. ~1,1 V (11 UVI), więc wejście jest nasycone.
- Historia z karty SD pokazuje narastanie od wschodu słońca: 05:00 → 0,17; 07:00 → 2,7 (max 7,3);
  08:00 → 15,3 (max 31,4); 09:00 → 23,9 (max 31,84). To sygnatura nasyconego stopnia wejściowego.
- Do sprawdzenia multimetrem: **VCC modułu = 3,3 V** (nie 5 V), napięcie na OUT ≤ 1,1 V,
  po zakryciu czujnika spadek do ~0.
- Światło: BH1750 20 474 lx (patrzy na horyzont) vs VEVOR 2 157 lx (w osłonie, do góry) —
  ta różnica jest normalna przy pełnym słońcu.

---

## AKTUALIZACJA (12.10, 08:15) — tryby wyświetlacza, grafiki pogody z SD, tryb nocny

- **Tryby ekranu (extra `disp_mode`), przełączane na żywo przez `POST /api/display`:**
  `0` = pulpit (dotychczasowy), `1` = **cykl dużych wskazań** (kolejne kanały z `disp_ch`,
  zmiana co `disp_step_s`), `2` = **jedna wartość na stałe**, `3` = **pogoda**
  (grafika słońce/chmury/deszcz/śnieg/mgła/noc + wartości z `disp_ch`).
- Nowe klucze: `disp_ch` (lista kanałów, tekst), `disp_font` (0 auto, 1 Font4, 2 Font6, 3 Font8,
  4 siedmiosegmentowa, 5 Font2), `disp_step_s` (2–120), `disp_night` + `disp_nf`/`disp_nt` (godziny),
  `disp_ic_x`/`disp_ic_y` (pozycja grafiki), `disp_i_sun/cloud/rain/snow/fog/night` (ścieżki BMP na SD).
- **Grafiki z karty SD:** plik **BMP** (np. 100×100, 24-bit) jest wczytywany **raz** do PSRAM
  (`LGFX_Sprite`) — w pętli nie ma potem żadnego odczytu z karty. Brak pliku / zły format =
  grafika rysowana wektorowo, więc tryb „pogoda" działa zawsze. Doszło `SdCardManager::fs()`
  (biblioteki zewnętrzne czytają pliki same, np. `drawBmpFile`).
- **Tryb nocny to kolory, nie podświetlenie:** GMT024-10 ma 7 pinów i **nie wyprowadza pinu
  podświetlenia (BLK)**, więc PWM jest niemożliwy. „Noc" (godziny `disp_nf`–`disp_nt`) wymusza
  ciemne tło i mnoży barwy ×0,38; prawdziwe ściemnianie dopiero po dołożeniu pinu BLK.
- **Stan pogody** liczony z: `rain`, `cam_rain`/`cam_snow`/`cam_fog`/`cam_cloud`, `hum`, `wind`,
  `temp` i pory dnia (kolejność: deszcz → śnieg → mgła → chmury → noc → słońce).
- **Sprawdzone na urządzeniu (08:10–08:15):** `mode=1` przechodzi temp → hum → press → c3_pm25 →
  c3_pm10 (co 5 s, `channel_now` się zmienia), `mode=2` i `mode=3` działają, `night_now` liczy się
  z zegara, pełny redraw 73–93 ms, `GET /api/display` pokazuje cały stan (mode, channel_now,
  weather, icon_ok, night_now). Stacja odpowiada 200, karta SD zamontowana (brak katalogu `/tft`).
- Nowa sekcja „Widok na ekranie" w zakładce TFT (tryb, lista kanałów z checkboxami, czcionka,
  cykl, noc, pozycja i ścieżki grafik) — **wymaga `uploadfs`, jeszcze nie wgrane**.
- **Nowy endpoint `POST /api/sd/upload?path=/tft/plik.bmp`** (surowe bajty w treści żądania,
  tylko `.bmp`, max 512 KB, katalog tworzy się sam, `reloadIcons()` po zapisie) + przycisk
  „Wgraj na kartę" w zakładce TFT. Sprawdzone: `/tft/sun.bmp` 30 054 B wgrany i wczytany jako
  grafika (log: „grafika /tft/sun.bmp (100x100 px) wczytana z karty SD", `icon_ok.sun = true`).

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
11. **Awaria 2026-10-11 — przyczyna ustalona (to NIE była kasacja NVS):** po `uploadfs` konfiguracja aplikacji w NVS (namespace `stacja`, klucze `cfg_0`/`cfg_c`/`cfg_ok`) miała **puste `wifi_ssid` i `wifi_pass`**, więc `setupWiFi()` wykonywało `return` **przed** `WiFi.begin()` → master wstawał tylko jako AP (`192.168.4.1`), bez STA → brak go w sieci domowej (HTTP 000, brak ARP). NVS **nie było skasowane** (obecne m.in. `boots`, `sta.ssid`/`sta.pswd` sterownika Wi-Fi). **Fix:** w `src/main.cpp` sprawdzenie pustego SSID przeniesione **za** blok statycznego IP + fallback `WiFi.begin()` (bez argumentów = poświadczenia z NVS sterownika Wi-Fi). Następnie `POST /api/config`: `wifi_static=true, wifi_ip=192.168.1.143, wifi_gw/wifi_dns=192.168.1.1, wifi_mask=255.255.255.0`.
12. **Ręczne patchowanie NVS odpada:** CRC wpisu NVS nie udało się odtworzyć standardowym CRC32 (0/122 trafień) — **nie patchować NVS z ręki**. Ustawienia zmieniać wyłącznie przez API urządzenia (`POST /api/config`).

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

### Sesja 2026-10-11 (kontynuacja) — ratowanie mastera
- Diagnoza przez USB (COM9: esptool `read_flash` + odczyt logu bootowania): master **wstawał**, ale bez STA (tylko AP) — przyczyna: puste Wi-Fi w konfiguracji aplikacji (pkt 6.11).
- **Fix w `src/main.cpp`** (jedyny plik kodu zmieniony w tej sesji): pusty SSID → `WiFi.begin()` na poświadczeniach z NVS sterownika Wi-Fi; statyczny IP stosowany w obu ścieżkach. Build 2× SUCCESS → flash przez USB (`pio run -e esp32s3-master -t upload --upload-port COM9`, app @0x10000; **NVS i LittleFS nietknięte**).
- `POST /api/config` → stały adres `192.168.1.143`. Weryfikacja: `/` → 200, `/api/status` → 200, 11 kanałów `c3_*`.

### Funkcje mastera (z tej sesji)
- **ExtDev/VEVOR:** backoff 60 s po 3 błędach / 300 s po 10, timeouts connect 2200 ms / read 1800 ms, budżet 4 s na wywołanie, `serviceTuya` czyta max 256 B, filtr `isfinite` przed ingestem/publikacją.
- **Daily restart** (opcjonalny): checkbox + godzina 00–23 na stronie Config, NVS (`extra.auto_restart`, `extra.auto_restart_hour`), warunki z pkt 9 powyżej. Endpoint `GET/POST /api/watchdog`.
- **Heartbeat ~1 Hz** na pinie `wd_heartbeat` (domyślnie −1) dla zewnętrznego modułu odcinającego zasilanie; działa też podczas OTA.
- **Sekcja „Stacja powietrza zewnątrz"** (c3_*) na pulpicie — kod zrobiony. W `/api/status` potwierdzone 11 kanałów: `c3_pm1, c3_pm25, c3_pm10, c3_temp, c3_hum, c3_press, c3_dew, c3_heat, c3_alt, c3_aqi, c3_pm25_60m`. **Grep sekcji w serwowanej stronie niedokończony** (patrz pkt 9).

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

1. **Master działa na 192.168.1.143** — nie ruszać bez potrzeby.
2. ✅ **ZROBIONE (11.10, 06:15)** — sekcja „Stacja powietrza zewnątrz" zweryfikowana: serwowany `index.html` **co do bajtu** równy lokalnemu (349 251 B, MD5 `FA6BE41110B499FB66F83D7F4AA6DA09`), `addZone('🌬️ Stacja powietrza zewnątrz', c3List, 'out')`, 11 kanałów `c3_*` z wartościami. **Bez screenshotów.**
3. ✅ **MQTT mastera WŁĄCZONE (11.10, 23:50)** — `POST /api/mqtt/test` (ustawienia w treści → `applyJson` + `reconnectWithConfig`) → `ok:true`, „Połączono z 192.168.1.14:1883 (27 ms)"; u brokera **41 stanów** + **41 discovery** HA. Bez restartu, bez flash.
4. ✅ **Build `esp32s3-master` — 2× SUCCESS** (RAM 26,7 %, Flash 56,5 %, 0 ostrzeżeń/błędów); backup `C:\Projects\NOWY BACKUP\ESP\StacjaPogody.zip` (44,6 MB, bez `.pio`, z `.git`).
5. ⏸️ **Push — WSTRZYMANY NA ŻYCZENIE UŻYTKOWNIKA** („nic na githuba niewgrywaj"). Zmiany **zacommitowane lokalnie** (11.10 23:50); lokal **ahead 20 / behind 8** vs `98e5aaa` — treść identyczna, push wymaga `--force-with-lease` albo merge. Sync z `C:\Projects\OnlineWorkspace\StacjaPogody\` **niewykonany**.
6. (opcjonalnie) Ustawić `wifi_ssid`/`wifi_pass` w konfiguracji mastera przez `POST /api/config`, żeby zakładka „Sieć" pokazywała realne dane (teraz puste — działa fallback na poświadczenia z NVS sterownika Wi-Fi).

---

## 10. PREFERENCJE UŻYTKOWNIKA (ważne!)

- Komunikacja **po polsku**, zwięźle — użytkownik jest niecierpliwy.
- **ŻADNYCH screenshotów / podglądów strony** — to najdroższe w tokenach. Weryfikacja tylko przez `curl` + `grep` / kod.
- **ZAKAZ drogich operacji** (oszczędność tokenów): brak screenshotów/`readPage`/przeglądarki, brak wczytywania i przeliczania całych wielkich plików „na wszelki wypadek" (czytać wąskie zakresy), brak dublowania komend.
- **ZAKAZ grzebania bez powodu i „głupiego" psucia:** działa → nie ruszać; najpierw przeczytać kod, potem działać; nie poprawiać „przy okazji".
- **OTA/FLASH — obowiązkowa kolejność:** najpierw sprawdzić firmware/kod i błędy, **dopiero potem** OTA. Build **dwukrotnie** (SUCCESS) + przegląd `git diff` + sprawdzenie, co i gdzie leci. Domyślnie wgrywać **tylko partycję aplikacji**; `uploadfs` bez wyraźnej potrzeby — NIE. Po wgraniu sprawdzić log bootowania.
- **ZAKAZ wgrywania czegokolwiek bez uprzedniego sprawdzenia kodu i przejścia buildu** (upload/uploadfs/flash). Najpierw czytaj kod + `pio run`, dopiero potem flash.
- **Przed OTA/uploadfs weryfikuj firmware DWUKROTNIE** (build SUCCESS + przejrzenie logu/zmian), zanim cokolwiek wgrasz. Zawsze sprawdzaj, co robisz, zanim to zrobisz.
- Nie przeliczać bez potrzeby setek tysięcy linii kodu; pracować minimalnie i celnie.
- Nowy czat na nowy temat/projekt (oszczędność tokenów); ten plik + checkpointy umożliwiają wznowienie.
- Mastera **nie** ma pod USB — aktualizacje przez OTA. (2026-10-11: master był tymczasowo podłączony pod USB na COM9.)
- **Kategoryczny zakaz kasowania / psucia NVS** i jakichkolwiek partycji danych. Nie wolno używać `erase_flash`, `factoryreset`, `nvs_flash_erase`. Ustawienia zmieniać **wyłącznie** przez `POST /api/config`.
- Nie wgrywać `uploadfs` bez wyraźnej potrzeby — użytkownik wiąże z nim awarię mastera.

## 11. SEKRETY

- Hasło MQTT (user `StacjaP`) i hasła WiFi są w pliku sekretów projektu — **nie drukować w czacie, nie commitować do gita**. Broker: 192.168.1.14:1883.
- WiFi mastera: SSID "<twoja-siec>", static 192.168.1.143, brama .1.
- Poświadczenia Wi-Fi są też zapisane w NVS sterownika Wi-Fi (`sta.ssid`, `sta.pswd`) — firmware używa ich jako fallbacku.
- **Nie zapisywać haseł w plikach na PC** (użytkownik zabronił). Zrzuty NVS trzymać wyłącznie w `%TEMP%` i **usuwać natychmiast** po użyciu.
