# StacjaPogody — Pamięć sesji (do wznowienia w nowym czacie)

> Ten plik to pełna kopia wiedzy z sesji zakończonej **2026-10-11 ~06:25**, dopisywana w kolejnych sesjach
> (ostatnia aktualizacja: **2026-10-12 22:27**).
> Aby kontynuować pracę w nowym czacie, powiedz agentowi:
> **„Przeczytaj `StacjaPogody.md` w głównym katalogu repo i kontynuuj od sekcji STAN OBECNY i DO ZROBIENIA."**

---

## 1. STAN OBECNY (najważniejsze — 2026-10-12 22:27)

| Urządzenie | IP | Stan |
|---|---|---|
| **Master** (ESP32-S3 WROOM-1 N16R8) | **192.168.1.143 (statyczne)** | ✅ **DZIAŁA** — `/` → 200, `/api/status` → 200. MAC `80:b5:4e:c6:52:a8`. Podłączony kablem USB do PC (COM9, USB-Serial/JTAG). |
| **Air node** (od 13.10.2026 **ESP32-WROOM-32U**, klasyczny ESP32; wcześniej C3, potem S3) | **192.168.1.147 (statyczne)** | ✅ **DZIAŁA** — WiFi `SGC` z anteną zewn. (**RSSI −28 dBm**, moc 19,5 dBm, bez usypiania), MQTT `stacja_c3`, konsola **COM19** (CP210x, 115200). Czujniki: AHT20 OK, BMP280 OK (0x77), **PMS7003 = 1 ramka/s (98/98 dobrych)** → `pm1/pm25/pm10 + temp/hum/press/dew/absh/heat/alt/aqi` (14 kanałów). Master importuje jako ExtDev `outaq` → kanały `c3_*` (`ok=True, 14 odczytów`). Log węzła: `/log.txt`, błędy: `/api/errs`. |
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

Do tego samodzielny węzeł **air node** (`c3air/`): AHT20 + BMP280 + PMS7003, WiFi + własna strona WWW +
MQTT + OTA + metryki pochodne. Ten sam kod buduje się na **4 płytki** (C3 4 MB, S3 16 MB, S3 8 MB, klasyczny
ESP32 4/8 MB) — różnią się tylko tabelą partycji i pinami/UART (patrz sekcja 5 i 4).

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

### Sekcja błędów na www + log do kopii (13.10, 17:40)
- **Nowa sekcja „Błędy i ostrzeżenia"** na pulpicie (`/`, tylko te, które wystąpiły) i w `/diag` (wszystkie
  kategorie): WiFi (rozłączenia z powodem), MQTT, czujniki I2C, pyłomierz (zła długość/suma, brak ramek),
  LittleFS, HTTP do mastera, OTA. Każda ma licznik, czas ostatniego wystąpienia i komunikat; do trwałego logu
  trafia 1. wystąpienie i co 50.
- **Nowe endpointy:** `/api/errs` (JSON z licznikami) i **`/log.txt`** (surowy log trwały + bieżący z RAM) —
  z tego master może robić kopię na SD (np. raz dziennie).
- **Ustawienia radia widoczne w `/diag`:** `Moc nadawania = 19,5 dBm`, `Usypianie modemu = wyłączone`,
  `RSSI = −28 dBm` (z podłączoną anteną zewnętrzną).
- **Piny zapasowe** dla klasycznego ESP32: **GPIO32 i GPIO33** (oba ADC1, nadają się na analogowy albo
  1-Wire, np. własny deszczomierz z kontaktronem) — widoczne w sondzie pinów w `/diag` i w logu startowym.
- **OTA działa normalnie** (wielokrotnie użyte 13.10, ostatnio za pierwszym razem). Zamieszanie z „obraz nie
  działa" dotyczyło **zapisu po USB**, nie OTA — patrz pułapka 3 wyżej.

---

## ZROBIONE (13.10, 19:35) — wyświetlacz TFT mastera (zgłoszone 13.10, 17:44)

Wszystko wdrożone i **wgrane na stację** (firmware OTA 19:2x, `verify_ok:true`, obraz 1 914 272 B):

1. **Górny pasek: TRZY znaczniki stanu naraz** (wspólne dla wszystkich widoków, zawsze widoczne):
   `KAMERA` (moduł ESP32-CAM), `C3` (węzeł powietrza na zewnątrz — ExtDevice typ `outaq`, .147),
   `WEW` (węzeł powietrza w środku — typ `aq`, .141). Kropka stanu (zielona / czerwona / szara) jest
   **na KOŃCU opisu**, promień 5 px, żeby kolor było widać z 2-3 m. Po prawej licznik cyklu (`7/13`) i zegar.
   Zestaw opisów dobiera się sam do szerokości panelu (na 320 px wchodzi `KAMERA / C3 / WEW`).
   Stan bierzemy **wyłącznie z tego, co urządzenia same przysyłają**: `extdev.ok` + `lastMs` + `peerMs`
   (`uptime_s`/`mqtt` z `/api/status` węzła), kamera z chwili odebrania zdjęcia — **bez adresów IP na sztywno**.
2. **Środek ekranu: rotacja wskazań** co `disp_step_s` (5 s) po liście `disp_ch` — obecnie 13 kanałów:
   `temp,hum,press,c3_temp,c3_hum,c3_pm25,c3_aqi,in_air_t,in_air_h,in_pm25,in_iaq,vev_wind,vev_rain_day`
   (czyli różne czujniki: master, węzeł C3 i stacja powietrza wewnętrzna). Duża wartość + jednostka
   **tuż za liczbą** + duży opis, czego dotyczy („Temperatura", „PM2.5 (C3)" itd.).
3. **Kolory temperatury** (`tempColor565`): poniżej 10 °C błękity (im zimniej, tym ciemniej, przy minusie
   granat), potem żółty → pomarańczowy → czerwony. Działa dla każdego kanału temperatury (`temp`, `c3_temp`, `in_air_t`).
4. **Przecinek dziesiętny** — czcionka 7-segmentowa (Font7) nie ma glifu przecinka, więc przy wartości
   z przecinkiem `setBigFont()` sam bierze Font8. Koniec z „17 2".
5. **Dolny pasek jakości powietrza** — 6 pasów o progach 0/50/100/150/200/300/500 (te same wartości hex,
   co legenda na stronie www) + słupek pokazujący aktualny AQI (`c3_aqi` → PM2.5 z C3 → `aqi` mastera).
6. **Nic na siebie nie nachodzi:** kropki cyklu na `H-38`, stopka na `H-20`, pasek na `H-12…H`.

Pliki: `src/drv_display.cpp`, `src/drv_display.h` (`nightK_`), `src/extdev.cpp`/`.h` (peer `uptime_s`/`mqtt`),
`src/camera.h` (`lastIngestMs()`). Sprawdzone na urządzeniu: `GET /api/display` — rotacja
`temp → c3_hum → c3_aqi` co 5 s, `last_draw_ms` 76-77 ms; `GET /api/extdev` — 3/3 `ok:true`.

---

## AKTUALIZACJA (13.10, 17:45) — dzienna kopia logów węzłów na SD
- Węzeł wystawia surowy log pod `/log.txt` (patrz `c3air`: `handleLogTxt` → log trwały z LittleFS + bieżący z RAM).
- Master raz na dobę (po 3:00 czasu lokalnego) pobiera `<baza>/log.txt` z każdego ExtDev typu `aq`/`outaq`
  i **dopisuje** na kartę SD do `/logi/<nazwa>-<RRRR-MM-DD>.txt` z nagłówkiem (adres + data kopii).
  Powtórny przebieg tego samego dnia nic nie duplikuje (sprawdza, czy plik już jest), a urządzenia bez
  `/log.txt` (np. .141) są pomijane jako „brak logu", nie jako błąd.
- Wołane w `main.cpp` zaraz po `extdev.loop()`. Wynik widać w syslogu: `Kopia logów węzłów: …`.
- **Sprawdzone na urządzeniu:** `/logi/Stacja_C3__powietrze_-2026-10-13.txt` — 9431 B, w środku logi
  startowe węzła z jego numeracją uruchomień.
- **OTA mastera:** `pio run -e esp32s3-ota -t upload --upload-port 192.168.1.143` — **adres IP jest konieczny**,
  bo domyślny `upload_port` w env to `stacja-pogody.local` i mDNS na tym PC nie rozwiązuje nazwy
  (`Host stacja-pogody.local Not Found`). Auth: `stacja-ota` (w env).

---

## AKTUALIZACJA (13.10, 13:30) — węzeł powietrza DZIAŁA na ESP32-WROOM-32U (COM19)

**Stan końcowy (zweryfikowany):** `192.168.1.147` — WiFi `SGC` STA, `RSSI −39 dBm`, MQTT → `192.168.1.14:1883`
(prefix `stacja_c3`), AHT20 OK, BMP280 OK (0x77), **PMS7003 = 1 ramka/s, 98/98 dobrych** (`pm1=7, pm25=9,
pm10=10 µg/m³`), 14 kanałów ważnych, master .143 importuje je jako `c3_*` (`ok=True, 14 odczytów`).

### Błąd, przez który pyłomierz „nie działał" (naprawiony)
W `pmsParse()` było:
```cpp
if (pmsRd() != 0x42) continue;
if (!PMS_SERIAL.available()) return;    // <-- gubił ramkę
```
Gdy w buforze była chwilowo tylko `0x42` (drugi bajt nagłówka jeszcze nie dotarł — pętla sprawdza to tysiące
razy na sekundę), funkcja **wychodziła ze zjedzonym bajtem nagłówka**. Efekt: **1 poprawna ramka na 45 s**
zamiast 45, a `bajty` w diagnostyce pokazywały poprawny ruch (~32 B/s). Naprawa: krótkie oczekiwanie (20 ms)
na drugi bajt i `continue`, zamiast `return`. **Pomiar po poprawce: 98 nagłówków / 98 dobrych, 0 błędów.**

### Diagnostyka dodana w `c3air` (przyda się przy kolejnych czujnikach)
- `/diag` → sekcja **„PMS: rozbiór"** (nagłówki / zła długość / zła suma / dobre / suma ramki vs wyliczona /
  początek ramki w hex) oraz **„Aktywność pinów czujników"** (liczba zmian poziomu w 200 ms dla 16, 17, 25, 26,
  27, 34). Uwaga: sonda 200 ms **nie łapie** ruchu pyłomierza (ramka zajmuje ~33 ms z każdej sekundy) — nie
  wyciągać z niej wniosku „brak sygnału", patrzeć na licznik bajtów.
- Znacznik `build <data> <godzina>` w logu startowym — pokazuje, **który obraz faktycznie działa**.
- Auto-wybór kierunku linii PMS: nasłuch obu pinów przy starcie + co 20 s (zamiana **tylko** gdy drugi pin ma
  wyraźnie więcej ruchu; działające wpięcie zapisywane w NVS jako `pmsSwap`).

### Piny pyłomierza — zmierzone, nie zgadywane
`PMS: ruch na 16 (18) / 17 (0)` → **RX płytki = GPIO16 (TX modułu), TX płytki = GPIO17 (RX modułu)**.

### Pułapki tej płytki (ESP32-WROOM-32U, CP210x, COM19, MAC `d4:e9:f4:a3:c5:0c`)
1. **EN jest na linii RTS** mostka (przy trzymaniu RTS-a log milknie = płytka w resecie).
2. **IO0 (BOOT) NIE jest podłączony** do DTR — przycisk BOOT nie ściąga IO0 do masy (6/6 prób dało
   `Wrong boot mode 0x13`). Wejście w tryb wgrywania: **zewrzeć pin `IO0`/`0` do `GND`** (kabelkiem) i wtedy
   zresetować EN. Działa sekwencja: trzymaj EN nisko (RTS=1), zewrzyj IO0→GND, puść EN → `default_reset`.
3. **Zapis po USB na `0x10000` nie działa, gdy `otadata` wskazuje drugą partycję** (np. po OTA). Objawy:
   „wgrałem, a działa stary obraz". Rozwiązanie: `erase_region 0xe000 0x2000` przed zapisem albo `--full`
   (bootloader + partycje + boot_app0, co resetuje `otadata`).
4. `setRxBufferSize()` **po** `begin()` nic nie robi (rdzeń tylko loguje błąd) — ustawiać przed `begin()`.

### Paniki z 13.10 (13:00–13:45) — co ustalono
Objaw: `Guru Meditation` (głównie `IllegalInstruction`) co ~20 s, w **losowych** miejscach (WiFi `lmac*`/`sta_*`,
lwIP `tcp_output_segment`, idle task, `PubSubClient::connected`), licznik uruchomień rósł do ~570.
Sprawdzone i **wykluczone**: rozmiar stosu pętli (16 KB nie pomógł), MQTT (przy wyłączonym też panikowało),
moc radia (11 dBm + usypianie też panikowało). **Ustąpiły po odłączeniu i ponownym podłączeniu USB** i od tego
czasu (13:00) płytka chodzi stabilnie (2× po 90 s i 100 s bez ani jednej paniki) na maksymalnej mocy.
Wniosek: nie kod, a przejściowy stan sprzętowy (zablokowane IO / „zawieszony" układ). **Jeśli wróci — najpierw
odłączyć i podłączyć USB, dopiero potem szukać w kodzie.**

### Pozostałe zmiany w kodzie
- `esp_ota_mark_app_valid_cancel_rollback()` przeniesione na **początek** `setup()` (wcześniej na końcu — gdy nowy
  obraz panikował w pierwszych sekundach, bootloader cofał aktualizację i „wgrane poprawki znikały").
- `-DARDUINO_LOOP_STACK_SIZE=16384` w `esp32-air-4mb` i `esp32s3-air`.
- Sonda pinów + licznik rozbioru ramek + znacznik builda (patrz wyżej).

---

## AKTUALIZACJA (13.10, 09:20) — drugi węzeł powietrza: ESP32-WROOM-32U (klasyczny ESP32)

- **Nowa płytka:** ESP32-D0WD-V3 rev 3.1, **4 MB**, mostek **CP210x (COM19)**, MAC `d4:e9:f4:a3:c5:0c`
  (krótsza obudowa + gniazdo anteny zewnętrznej). Fabrycznie miała obcy układ partycji (recovery/ota_0/settings
  z ESP-IDF 5.3.1) i pętlę resetów TG0WDT.
- **Piny (środowisko `esp32-air-4mb`)** — w `c3air/src/main.cpp` pod `C3AIR_ESP32_CLASSIC`:

| Funkcja | GPIO |
|---|---|
| I2C1 SDA / SCL (AHT20 `0x38`, BMP280 `0x76/0x77`) | **21 / 22** |
| I2C2 SDA / SCL (przyszły czujnik zewnętrzny) | **25 / 26** |
| PMS7003 RX / TX (`Serial2`, 9600) | **16 / 17** |
| Deszcz — linia analogowa (AO) | **34** (ADC1_CH6, tylko wejście) |
| Wiatromierz — hall D0 | **27** (pull-up + przerwanie) |
| wolne / rezerwa | 4, 5, 18, 19, 23, 32, 33, 35, 36, 39 |

  Uzasadnienie: 6–11 = flash, 0/2/12/15 = strapping, a **ADC2 (0,2,4,12–15,25–27) nie działa przy Wi-Fi** —
  dlatego deszcz MUSI być na ADC1 (32–39). 25/26/27 są tam użyte cyfrowo, więc to nie przeszkadza.
- **Pułapka (kosztowała jedno wgranie):** klasyczny ESP32 ma bootloader na **`0x1000`**, nie `0x0` jak C3/S3.
  Pod `0x0` ROM wpada w pętlę `invalid header: 0x04202040` + `RTCWDT_RTC_RESET`. `tools/flash_retry.py`
  wybiera offset sam (`--chip esp32` → `0x1000`).
- **Pułapka sprzętowa tej płytki:** IO0 (BOOT) **i** EN **nie są podłączone** do DTR/RTS mostka CP210x —
  wejście w tryb wgrywania tylko przez **fizyczne trzymanie BOOT** w chwili resetu, a po wgraniu trzeba
  nacisnąć EN albo odłączyć USB (inaczej zostaje `waiting for download`).
- **Stan po wgraniu (zweryfikowany):** `piny: I2C1 SDA=21 SCL=22 | I2C2 SDA=25 SCL=26 | PMS RX=16 TX=17 |
  deszcz ADC=34 | hall=27`; WiFi `SGC` → `192.168.1.147`; MQTT połączony (prefix `stacja_c3`); konsola UART0
  (COM19, 115200). Obie magistrale I2C **puste** — czujniki jeszcze niepodłączone.
- **Diagnostyka:** `/diag` pokazuje surowy odczyt linii deszczu (ADC), licznik impulsów halla i skan drugiej
  magistrali I2C. Bez kanałów MQTT i bez zmian w tabelach importu mastera (świadomie — czekają na decyzję
  o jednostkach/nazwach).
- **`--erase-nvs`** w `tools/flash_retry.py`: czyści NVS (`0x9000-0xE000`) przy pierwszym wgraniu na płytkę
  z obcym układem partycji. **Używać tylko dla nowej płytki — nigdy dla mastera.**

---

## AKTUALIZACJA (12.10, 22:27) — węzeł powietrza na ESP32-S3 (N16R8) + warianty płytek

- **Nowa płytka na COM12 = ESP32-S3, 16 MB flash** (MAC `80:b5:4e:c3:f8:58`, mostek CH343). Poprzedni węzeł
  C3 (MAC `10:B4:1D:16:25:14`) **nie łączył się z routerem SGC** (powód 15/202/204 mimo poprawnego hasła);
  na S3 **ten sam kod łączy się od razu** (RSSI **−43 dBm**) — hipoteza o słabym RF tamtej płytki potwierdzona.
- Wgrany **pełny obraz** (`--full`: bootloader `0x0`, partycje `0x8000`, `boot_app0` `0xe000`, app `0x10000`)
  środowiska `esp32s3-air`, z fabrycznymi danymi Wi-Fi wkompilowanymi przez `PLATFORMIO_BUILD_FLAGS`
  (nie trafiają do repo; pusta płytka zapisuje je do NVS w `loadCfg()`).
- **Stan .147 (zmierzony):** `/api/status` → `fw=c3air`, `mqtt:true`, IP statyczne `192.168.1.147`, `-43 dBm`;
  MQTT do `192.168.1.14:1883`, prefix `stacja_c3` (ustawione przez `POST /save`: `mqh/mqp/mqu/mqw/mqx`).
- **Konsola S3 przeniesiona na UART0** (usunięte `-DARDUINO_USB_CDC_ON_BOOT=1` z `esp32s3-air*`), bo płytka jest
  programowana i podglądana przez mostek CH343 — wcześniej nie było widać **żadnego** logu. PMS7003 siedzi
  teraz na **UART1** (RX 20 / TX 5), żeby nie kolidował z konsolą (makro `C3AIR_ESP32S3`).
- **5 wariantów firmware, wszystkie build SUCCESS:** `esp32c3-air` (4 MB, 54,7 %), `esp32s3-air` (16 MB, 33,2 %,
  build 2×), `esp32s3-air-8mb`, `esp32-air-4mb` (63,9 %), `esp32-air-8mb`. Klasyczny ESP32: I2C 21/22, pył na
  `Serial2` (16/17) — **nie ma GPIO20**, a konsola zajmuje UART0 (`C3AIR_ESP32_CLASSIC`).
- **Naprawione błędy w `c3air/src/main.cpp`:**
  1. `bmpBegin()` zapisywał `bmpAddr` **przed** sprawdzeniem ID → bez czujnika `/diag` pokazywał fałszywe
     „BMP280 OK (0x76)". Teraz adres zapisywany dopiero po potwierdzeniu ID `0x58`.
  2. Brak ponownego wykrywania czujników → dodane w `loop()` co 30 s, więc moduł podłączony po starcie
     działa **bez restartu** urządzenia.
- **Czujniki węzła: NIEPODŁĄCZONE** — `I2C: (puste)`, AHT20 brak, BMP280 brak, PMS7003 brak ramek. Master (.143)
  odpytuje .147 jako ExtDev `outaq` i słusznie zgłasza „brak rozpoznanych kanałów"; widoczne `c3_*` to
  **stare wartości** z poprzedniego węzła C3.
- **Narzędzie:** `tools/flash_retry.py --build <dir> --chip <esp32|esp32s3> --flash-size <4MB|8MB|16MB> [--full|--app-only]`
  (czeka na port, odrzuca „duchy" COM, próbuje `default_reset` i `no_reset`).
- **Pułapka (mój błąd, naprawiony):** podmiana tekstu przez PowerShell (`Get-Content -Raw` + `WriteAllText`
  w PS 5.1) **podwójnie koduje polskie znaki** (systemowy ANSI = cp1250): `ó` (`C3 B3`) → `Ăł` (`C4 82 C5 82`).
  Wykrywanie/cofanie bajtowo (liczniki `c3b3` vs `c482`) odwrotną tabelą cp1250. **Tekst edytować tylko narzędziem `edit`.**
- **Pułapka:** cudzysłowy w `PLATFORMIO_BUILD_FLAGS` muszą mieć `\` (`-DC3_DEF_PASS=\"hasło\"`), inaczej build
  pada na `expected primary-expression before '}'`.
- **Na życzenie użytkownika:** bez commita/pusha, bez syncu z `OnlineWorkspace`, bez backupu ZIP.

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
- **Wgrane 13.10, 19:24:** `uploadfs` — strona (374 588 B) i `i18n.js` serwowane z mastera, zgodne z plikami
  lokalnymi; w menu jest nowa zakładka „Węzły Wi-Fi" (druga sekcja obok ESP/RS485, której nie ruszano).
  Uwaga: pierwsza próba `espota -t uploadfs` padła na 38% i **zostawiła FS bez strony** — obraz FS
  (10 420 224 B) wgrywaj przez `POST /api/ota/filesystem` (multipart + `X-OTA-Key`), poszło w 88 s.

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
- `uv` = analogowy czujnik UV na `uv_adc` (GPIO4), wzór `UVI = mV / czułość`.
- **Objaw:** stale 31,84 (= 3184 mV, raw 4095/4095) mimo zmian światła 24 k → 54 k lx.
- **Diagnoza:** historia z karty SD pokazała **identyczne nasycenie dzień wcześniej** (13:00 → 31,81),
  a nocą poprawne ~0,3 — czyli sygnał z czujnika rośnie i w południe przekracza zakres wejścia.
  Test linii (`lo=4095 hi=4095`) potwierdził, że wyjście modułu siedzi na szynie 3,3 V
  (impedancja wyjścia wzmacniacza << 45 kΩ wewnętrznego ściągania do masy).
- **Przyczyna:** moduł ma **~10x większą czułość** niż założone 100 mV/UVI (typowe dla GUVA-S12SD).
- **Naprawa programowa:** czułość `uv_mv_uvi` = **1000 mV/UVI** (1 V na 1 UVI). Weryfikacja:
  południe 3184 mV → **3,2** (VEVOR: 3), 07:00 → 0,27, 17:00 → 0,17, 06:00 → 0,02 — zgadza się
  z porą dnia. Przy nasyceniu wejścia kanał **nie znika** (to dolne oszacowanie), a w logu leci
  ostrzeżenie raz na 5 min.
- Światło: BH1750 patrzy na horyzont, VEVOR w osłonie do góry — różnica jest normalna.

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

### Air node — warianty płytek (ten sam kod, 4 tabele partycji)
| Środowisko PlatformIO | Płytka | Flash | Partycje | Konsola / pył |
|---|---|---|---|---|
| `esp32c3-air` | ESP32-C3 SuperMini | 4 MB | `c3air/partitions.csv` | USB-CDC / UART0 (20, 5) |
| `esp32s3-air` | ESP32-S3 (N16R8) | 16 MB | `partitions.csv` | UART0 (CH343) / UART1 (20, 5) |
| `esp32s3-air-8mb` | ESP32-S3 N8 | 8 MB | `partitions-8mb.csv` | jak wyżej |
| `esp32-air-4mb` | klasyczny ESP32 (WROOM-32) | 4 MB | `partitions-4mb.csv` | UART0 / UART2 (16, 17), I2C 21/22 |
| `esp32-air-8mb` | klasyczny ESP32 | 8 MB | `partitions-8mb.csv` | jak wyżej |

Uwaga: klasyczny ESP32 **nie ma GPIO20**, a konsola siedzi na UART0 — dlatego pył idzie po `Serial2`
(16/17), a I2C na 21/22 (przełącznik `C3AIR_ESP32_CLASSIC` w `c3air/src/main.cpp`).

**Wgranie z retry (płytka wchodzi w download mode sama albo po resecie):**
```powershell
cd "C:\Projects\NOWY BACKUP\ESP\StacjaPogody"
# nowa/pusta płytka: --full (bootloader + partycje + app); aktualizacja: --app-only
& "...\python.exe" tools\flash_retry.py --build c3air/.pio/build/esp32s3-air --chip esp32s3 --flash-size 16MB --full
& "...\python.exe" tools\flash_retry.py --build c3air/.pio/build/esp32-air-4mb --chip esp32 --flash-size 4MB --full
```
Fabryczne Wi-Fi dla pustej płytki (nie trafia do repo, przekazywane przy budowaniu):
`$env:PLATFORMIO_BUILD_FLAGS = '-DC3_DEF_SSID=\"SGC\" -DC3_DEF_PASS=\"<hasło>\"'` — cudzysłowy **muszą** być
poprzedzone `\`, inaczej build pada (`expected primary-expression before '}'`).

**Pełny flash C3 po zmianie tabeli partycji** (bootloader+partitions+app, zachowuje NVS):
```powershell
C:\Users\kruse\.platformio\penv\Scripts\python.exe C:\Users\kruse\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32c3 -p COM24 -b 460800 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```
(Systemowy Python nie ma zależności esptool — `intelhex` brakuje; używać penv.)

---

## 5. PINSY I CZUJNIKI

### Air node — piny w zależności od płytki (`c3air/src/main.cpp`)
- **ESP32-C3** (SuperMini): I2C SDA=8, SCL=9; PMS7003 na UART0 RX=20 / TX=5, 9600; konsola USB-CDC.
- **ESP32-S3**: I2C 8/9; PMS na **UART1** RX=20 / TX=5; konsola UART0 (GPIO43/44 → mostek CH343).
  UART1 musi być wolny, bo konsola dzieli UART0 z wgrywaniem firmware.
- **klasyczny ESP32**: I2C SDA=21, SCL=22; PMS na **UART2 (Serial2)** RX=16 / TX=17; konsola UART0.
  (Brak GPIO20; `Serial0` = konsola, więc pył nie może na niego trafić.)
- Czujniki: AHT20 (0x38), BMP280 (0x76/0x77 — ID 0x58), PMS7003. Brak czujnika = `brak` w `/diag`
  (adres BMP280 zapisywany **po** potwierdzeniu ID), wykrywanie ponawiane co 30 s w pętli → można
  podłączyć czujnik bez restartu.

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
- `c3air/src/main.cpp` — cały firmware (pojedynczy plik, ~25 KB), wspólny dla wszystkich płytek
  (przełączniki `C3AIR_ESP32S3`, `C3AIR_ESP32_CLASSIC`).
- `c3air/platformio.ini` — 5 środowisk: `esp32c3-air`(+`-ota`), `esp32s3-air`(+`-ota`), `esp32s3-air-8mb`,
  `esp32-air-4mb`, `esp32-air-8mb`.
- `c3air/partitions.csv` (C3 4 MB), `partitions.csv` / `partitions-8mb.csv` / `partitions-4mb.csv` (root, wg flashu).
- `tools/flash_retry.py` — wgrywanie z retry (czeka na port, `--full`/`--app-only`, `--flash-size`).
- `src/wifi_util.h` — `wifiApplyMaxPower()` (pełna moc WiFi, wspólne dla wszystkich płytek).

---

## 9. DO ZROBIENIA (kolejność)

1. **Master działa na 192.168.1.143** — nie ruszać bez potrzeby.
2. ✅ **ZROBIONE (11.10, 06:15)** — sekcja „Stacja powietrza zewnątrz" zweryfikowana: serwowany `index.html` **co do bajtu** równy lokalnemu (349 251 B, MD5 `FA6BE41110B499FB66F83D7F4AA6DA09`), `addZone('🌬️ Stacja powietrza zewnątrz', c3List, 'out')`, 11 kanałów `c3_*` z wartościami. **Bez screenshotów.**
3. ✅ **MQTT mastera WŁĄCZONE (11.10, 23:50)** — `POST /api/mqtt/test` (ustawienia w treści → `applyJson` + `reconnectWithConfig`) → `ok:true`, „Połączono z 192.168.1.14:1883 (27 ms)"; u brokera **41 stanów** + **41 discovery** HA. Bez restartu, bez flash.
4. ✅ **Build `esp32s3-master` — 2× SUCCESS** (RAM 26,7 %, Flash 56,5 %, 0 ostrzeżeń/błędów); backup `C:\Projects\NOWY BACKUP\ESP\StacjaPogody.zip` (44,6 MB, bez `.pio`, z `.git`).
5. ⏸️ **Push — WSTRZYMANY NA ŻYCZENIE UŻYTKOWNIKA** („nic na githuba niewgrywaj"). Zmiany **zacommitowane lokalnie** (11.10 23:50); lokal **ahead 20 / behind 8** vs `98e5aaa` — treść identyczna, push wymaga `--force-with-lease` albo merge. Sync z `C:\Projects\OnlineWorkspace\StacjaPogody\` **niewykonany**.
6. (opcjonalnie) Ustawić `wifi_ssid`/`wifi_pass` w konfiguracji mastera przez `POST /api/config`, żeby zakładka „Sieć" pokazywała realne dane (teraz puste — działa fallback na poświadczenia z NVS sterownika Wi-Fi).
7. **Podłączyć czujniki do węzła powietrza .147** (ESP32-S3): I2C `SDA=8 / SCL=9` (AHT20 `0x38`, BMP280 `0x76/0x77`), PMS7003 na `UART1` (`RX=20 / TX=5`). Po podłączeniu czujniki są wykrywane w ciągu 30 s **bez restartu**, a master zacznie importować `c3_*`. Kolejność flashowania: kod → build 2× → `git diff` → dopiero potem wgranie (domyślnie tylko aplikacja `@0x10000`).
8. **Warianty klasycznego ESP32 (`esp32-air-4mb`, `esp32-air-8mb`) — zbudowane, niegrane.** Wgranie, gdy płytka będzie podłączona: `tools/flash_retry.py --build c3air/.pio/build/esp32-air-4mb --chip esp32 --flash-size 4MB --full`.

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
- WiFi mastera: SSID „SGC", static 192.168.1.143, brama .1.
- Poświadczenia Wi-Fi są też zapisane w NVS sterownika Wi-Fi (`sta.ssid`, `sta.pswd`) — firmware używa ich jako fallbacku.
- **Nie zapisywać haseł w plikach na PC** (użytkownik zabronił). Zrzuty NVS trzymać wyłącznie w `%TEMP%` i **usuwać natychmiast** po użyciu.
