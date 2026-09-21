# Stacja Pogody - stan prac i lista zadań

Aktualizacja: 2026-09-20 03:58
Zasady bez zmian: **nie kasujemy NVS**, **nie ruszamy Wi-Fi laptopa**, USB tylko COM9 (master)
i COM10 (węzeł C3) i tylko po wyraźnej zgodzie, w edytorze pinów nie klikamy
"Zapisz i zrestartuj", jeśli restart nie jest zamierzony.

## Zrobione

### Naprawa błędu, przez który master startował jako węzeł bez sieci (STACJA_HEADLESS)
Po wgraniu firmware przez USB master **zniknął z sieci** (brak `192.168.1.143` w ARP, brak
odpowiedzi HTTP). Log z COM9 pokazał, że płytka wstała jako węzeł RS485 bez Wi-Fi:
`Węzeł bez sieci: bez strony www`, `Tryb węzła RS485 (bez sieci): adres 1`,
`RS485: węzeł, adres 2, drugi ESP 2, 115200 b/s, piny RX 39 TX 38 DE 47`.

Przyczyna: `src/config.h` **zawsze definiuje** `STACJA_HEADLESS` (domyślnie `0`), a kod
sprawdzał makro przez `#ifdef` / `#ifndef`. `#ifdef STACJA_HEADLESS` było więc prawdziwe
w **każdym** środowisku, także w `esp32s3-master` - master kompilował ścieżkę węzła
(bez `setupWiFi()`, bez MQTT/OTA/www). Dowód: literały z `firmware.bin` (888 896 B) zawierały
`Węzeł bez sieci`, a nie zawierały `Strona www: index.html` ani `/api/pins`.

Naprawa (35 miejsc w 3 plikach): wszystkie testy zamienione na `#if STACJA_HEADLESS` /
`#if !STACJA_HEADLESS` - makro z `-D` daje 1 w `esp32c3-node`, a brak definicji daje 0
w pozostałych środowiskach:
- `src/main.cpp` - 17 miejsc,
- `src/rs485.cpp` - 7 miejsc,
- `src/api_json.cpp` - 6 miejsc **plus** nowy `#include <WiFi.h>` w bloku `#if !STACJA_HEADLESS`
  (kod sieciowy nigdy wcześniej się nie kompilował, więc brakowało nagłówka: `WL_CONNECTED`,
  `WiFi.status()`, `WiFi.RSSI()`),
- `src/config.h` - komentarz z zasadą: makro jest zawsze zdefiniowane, wolno tylko `#if`.

Sprawdzone w binarkach (dekodowanie UTF-8, bo polskie znaki): `esp32s3-master` 1 550 400 B
ma `Strona www: index.html`, `/api/pins`, `/api/ota/firmware`, nie ma `Węzeł bez sieci`;
`esp32c3-node` 769 440 B ma `Węzeł bez sieci`, nie ma `/api/pins`. Po wgraniu mastera przez USB
(tylko 0x10000, NVS nietknięte) stacja wróciła: `/api/ota/info` -> `running_partition: app0`,
`/api/pins` -> 17 348 B z nowymi opisami, `/` -> `index.html` (221 021 B),
`/api/rs485` -> `online: true`, `peer: 2`, `addr: 1`, `ok: 2`, `err_crc: 0`.

### Firmware (ESP32-S3 WROOM-1 N16R8 master + ESP32 jako węzeł RS485)
- Pełna stacja: czujniki I2C/1-Wire/ADC/SPI, PSRAM, logowanie na microSD, LED WS2812B,
  RTC DS3231, GPS (NEO-6M/M8N/M9N) jako źródło czasu, MQTT -> Home Assistant, OTA
  (firmware + LittleFS), RS485 dla węzłów z czujnikami zewnętrznymi.
- Architektura sterowników: kontrakt `src/drv_mod.h` (`ChanDef`, `DrvSink`, `DrvModule`),
  rejestr `src/drv_table.cpp` (8 slotów: ths, light, air, io, tc, motion, i2s, gas).
- **Wszystkie 4 nowe sterowniki dostarczone i podlinkowane** (weak extern w `drv_table.cpp`,
  0 zmian w rejestrze):
  - `src/drv_tc.*` - MAX6675/MAX31855 (K/J/N/T/S/R/E), MAX31865 (PT100/PT1000, 2/3/4 przewody,
    filtr 50/60 Hz), MCP3008/MCP3208 - SPI **bit-bang**, więc magistrala może siedzieć na
    dowolnych GPIO; 13 kanałów (`tc_*`, `rtd_*`, `adc_mcp_ch0..ch7`).
  - `src/drv_motion.*` - QMC5883L/HMC5883L, MPU6050/6500/9250/9255, LIS3DH, ADXL345,
    VL53L0X/VL53L1X, MLX90614; 19 kanałów (`mot_*`), deklinacja z `mag_decl`.
  - `src/drv_i2s.*` - INMP441/ICS-43434 (3 kanały `snd_*`), MAX98357A (TX w trybie ciszy,
    bez odtwarzania); brak pinów `i2s_*` = moduł nieinstalowany.
  - `src/drv_gas.*` - MQ/MiCS/TGS (16 modeli), ACS712/ACS758, mokrość liścia / oblodzenie,
    PAR; 14 kanałów (`gas_*`, `acs_*`, `leaf`, `ice`, `par`), ADS1115 opcjonalnie.
  - Razem 170 kanałów w firmware.
- Dynamiczna rejestracja kanałów (`src/sensors_extra.cpp`), nadpisania kanałów przez API.
- RS485: ramka 8192 B, stronicowanie odczytu węzła (`/api/node?c=chan`).

### Strona www (SPA w LittleFS)
- Menu **boczne, przewijane** z podmenu (Ustawienia -> Kalibracja, Piny, itd.), wersja PL/EN/DE.
- Zakładki m.in.: Pulpit, Czujniki, Kalibracja, Piny, ESP i magistrala RS485, LED, RTC, GPS, logi/SD.
- Edytor pinów: szablony dla płytek (ESP32-S3 N16R8/N8R4, ESP32-C3 SuperMini), 49 ról pinów,
  wykrywanie zajętości, opisy ról.
- Legenda czujników (`data/legend.js`, generowana z `docs/CZUJNIKI.md`): 225 wierszy,
  filtr, wyszukiwarka, znaczniki magistral (I2C/I2S/1W/SPI/UART/RS485/ADC/GPIO/INT),
  klik w pin -> podświetlenie w edytorze pinów. Statusy: **ok = 99, plan = 126**.
- **Na stronie widać wyłącznie realnie podłączony sprzęt** (nic nie jest jeszcze zlutowane,
  więc testowe/planowane wpisy zniknęły z widoku): pulpit pokazuje kanały `detected && enabled`
  (bez kafelków "czeka na dane", pusta lista daje komunikat "podłącz czujnik"), tabela
  **Czujniki** i **Kalibracja** filtrują po `detected`, legenda domyślnie pokazuje tylko
  czujniki ze sterownikiem w firmware. Każda z tych list ma przełącznik odsłaniający resztę
  (*Pokaż też nieaktywne* / *Pokaż też czujniki bez sterownika*), a sekcja
  "Zaawansowana kalibracja układów" pokazuje tylko grupy układów, które są wykryte
  (albo wpisane w pinach) - ukryte grupy zachowują swoje wartości w NVS, bo formularz
  wysyła wyłącznie widoczne pola. Kolumna "Stan" i znaczniki ✅/🟡/⛔ usunięte z legendy.
- Kalibracja analogowa (gleba, deklinacja, pyranometr) + **sekcja
  "Zaawansowana kalibracja układów"** (38 parametrów w 7 grupach: wzmocnienia/czas integracji
  TSL, skale UV, rezystory bocznikowe INA, kompensacje SCD4x/ENS160/SGP30, MS5611, oraz nowe:
  typ termopary + Rref/R0/przewody/filtr RTD + Vref ADC SPI, zakresy IMU, emisyjność IR,
  offset ToF, deklinacja, korekta poziomu mikrofonu/okno LEQ).

### Narzędzia
- `tools/gen_legend.py` - generuje `data/legend.js` z katalogu.
- `tools/sync_status.py` - synchronizuje statusy w `docs/CZUJNIKI.md` (🟡 -> ✅) na podstawie
  faktycznie istniejących kanałów i sterowników; dokładne dopasowanie tokenów, tabela `MANUAL`.
- `tools/check_i18n_names.js` - kontrola pokrycia tłumaczeń (kanały, piny: nazwy/grupy/opisy,
  pola kalibracji układów) + kontrola, że każdy klucz `extraF/hasExtra` z `src/drv_*.cpp`
  ma pole w formularzu `CALIB_ADV`. Wynik: 1365 kluczy i18n, wszystko OK
  (INFO: `snd_offset_db`, `snd_leq_s` czytane makrami, więc regex ich nie widzi).

### Piny i dokumentacja
- **DHT22/DHT11 - domyślny pin `GPIO 42`** (`include/pins.h`, rola `dht`): wolny pin złącza J3,
  grupa JTAG 39-42 (działa jako GPIO, dopóki nie używamy zewnętrznego debuggera), 40/41 zostają
  wolne. Uwaga: opcjonalny tryb SDMMC 1-bit (40/41/42) koliduje z tym pinem.
- Poprawione opisy pinów w `README.md` (tabela pinów + nota o N16R8/SDMMC) i `docs/CZUJNIKI.md`
  (wiersz DHT miał błędnie `GPIO 2`, czyli pin sondy gleby).
- **Magistrala RS485 - piny i okablowanie sprawdzone na sprzęcie** (115200 b/s, master = stacja).
  W obecnym układzie **nie ma konwertera MAX485** - to bezpośrednia linia TTL (3 przewody):
  | stacja (S3) | rola | węzeł (C3 SuperMini) |
  |---|---|---|
  | `GPIO 38` (TX) | -> | `GPIO 20` (RX węzła) |
  | `GPIO 39` (RX) | <- | `GPIO 21` (TX węzła) |
  | `GND` | wspólna masa (wymagana!) | `GND` |
  | `GPIO 45` DE/RE (na płytce ustawione `47`) | sterowanie kierunkiem | - |
  Przewody idą "po kolei", bez krzyżowania (master 38 -> RX węzła 20, master 39 -> TX węzła 21),
  bo C3 SuperMini ma UART0 na GPIO 20/21. Zasada nadrzędna: **RX jednego ESP na TX drugiego**.
  Konwerter MAX485/SP3485 (`A-A`, `B-B`, terminacja 120 Ω) jest potrzebny tylko wtedy, gdy
  kiedyś dojdzie do tego prawdziwa magistrala różnicowa np. dla czujników Modbus.
  Opisy ról w edytorze pinów podają piny węzła (`rs485_rx`: "ten pin do węzła C3 SuperMini
  GPIO 21 (TX węzła)", `rs485_tx`: "... GPIO 20 (RX węzła)"), a `docs/CZUJNIKI.md` ma
  poprawiony wiersz Modbus (`TX 38`, `RX 39`, `DE 45`).
  Stan po wgraniu (2026-09-20): `/api/rs485` -> `online: true`, `node_count: 1`, `addr: 2`,
  `baud: 115200`, `de: 47`, `tx/rx/ok: 17/15/15`, `raw_rx: 47695`, `latency_ms: 195`,
  `mirrored: 169/179`; `?a=sniff&pin=38` -> `idle: 100`, `drive: 52`. Węzeł zgłasza
  `board: c3mini`, `chip: ESP32-C3`, `fw 1.0.0 (node)`, `heap 174368`.
- **Węzły na magistrali (zaimplementowane)**: master trzyma w NVS **listę węzłów**
  (maks. `RS485_MAX_NODES` = 8, adresy 2–32) i odpytuje je kolejno z odstępem min. 2 s.
  Adres zmienia się **przez magistralę** z www (`add` / `del` / `assign` / `scan`, zakładka
  **ESP i magistrala RS485** → *Lista węzłów*). Są też **gotowe binarki z adresem**:
  `…-node-c3-4mb-addr3.bin` … `-addr8.bin` (flaga `-DSTACJA_NODE_ADDR=N`, adres fabryczny
  w `src/config.h` = 2). Na **świeżym** module (pusta NVS) dają adres od razu, więc kilka
  nowych modułów nie koliduje na magistrali; adres nadany potem z www (w NVS) ma pierwszeństwo.
  Każdy węzeł ma własne podmenu *ESP #n* w menu bocznym
  i sekcję `ESP #n (Adres m)` na pulpicie, a jego kanały są lustrami `xN_<id>` na masterze.
  Węzeł C3 zbiera tylko dane z podłączonych u niego czujników (magnetometr/kontaktron anemometru,
  czujnik Hall deszczomierza, czujnik płytkowy deszczu/śniegu) i odsyła je do mastera;
  master loguje wszystko na microSD i wysyła dalej (www, MQTT/HA).
- **Czujniki węzła C3 (do podłączenia)**: `AS5600` - SDA `GPIO 8`, SCL `GPIO 9` (kierunek
  wiatru), kontaktron/Hall anemometru - `GPIO 5`.

### Aktualizacja OTA (www + espota) - działa, trzy przyczyny usunięte
Objawy były trzy i wszystkie są już naprawione (`src/ota.cpp`, `src/flashfix.cpp`):

1. **"Could Not Activate The Firmware"** - `esp_image: invalid segment length 0x676e696d`
   ("ming" z `spi_timing_config.c`) wyskakiwał, bo odczyt z flash z wyłączonym cache
   (`esp_partition_read`) zwracał uszkodzone 32 B, gdy adres nie był wyrównany do 32 B.
   Dopisany skrót SHA-256 obrazu czyta się właśnie takim odczytem (offset 0x18), więc
   aktywacja odrzucała poprawny plik. `src/flashfix.cpp` podmienia odczyt na porcje
   wyrównane do 32 B (`/api/ota/info` -> `flash_read_fix: true`). Sprawdzone w obie
   strony (app0 <-> app1), `last_ota_patched: false` potwierdza, że obejście geometrii
   partycji nie było potrzebne.
2. **Zatrzymanie wgrania w połowie** - `Update.begin(UPDATE_SIZE_UNKNOWN, ...)` przy
   nieznanym rozmiarze przyjmuje **całą partycję** (3 MB app / 9,94 MB spiffs) i kasuje ją
   z góry, blokując transport (pierwszy pomiar: zatrzymanie na ~565 kB).
   Teraz rozmiar bierzemy z `Content-Length` przeglądarki, a dla systemu plików przycinamy
   go do rozmiaru partycji (`UpdateClass::begin()` odrzuca `size > partycja`). Efekt:
   `przygotowanie 0 ms`, firmware 1,5 MB w ~13 s, LittleFS 9,94 MB w 72 s (~145 kB/s),
   `{"ok":true}` w obu przypadkach.
3. **Spontaniczne przerwanie wgrania** ("cisza 0 ms", losowe offsety 3,68 / 4,18 / 7,66 MB) -
   znaczniki czasu wgrywania (`s_upActive`, `s_upLastMs`) czyta pętla główna, a zapisuje
   zadanie serwera asynchronicznego; bez `volatile` pętla widziała nieaktualną wartość
   i przerywała poprawne wgranie. Zmienne są `volatile`, a ciszę potwierdza **drugi pomiar
   po 500 ms** (`OTA_UP_IDLE_CONFIRM_MS`), więc pojedynczy stary odczyt nie może już
   przerwać transmisji. Po zmianach: 3/3 pełne wgrania bez fałszywego przerwania.

Zabezpieczenia (przydatne, gdy przeglądarka zamknie kartę albo padnie Wi-Fi):
`/api/ota/info` pokazuje `upload_active`, `upload_bytes`, `upload_idle_ms`, `upload_timeout_ms`
i `upload_aborted`; cisza > 20 s lub rozłączenie klienta przerywają zapis, remontują LittleFS
i nie blokują już pętli głównej (`/api/reboot` działa po przerwaniu - wcześniej stacja wisiała).
Diagnostyka: `GET /api/ota/diag`.
Uwaga przy testach: **otwarcie COM9 (monitor) zeruje płytkę**, więc nie wolno trzymać portu
szeregowo otwartego w trakcie wgrywania OTA; `espota` (ArduinoOTA) działa po **adresie IP**
(`stacja-pogody.local` nie zawsze się rozwiązuje), hasło `stacja-ota`.

### Identyfikacja portów COM i wgranie obu płytek (2026-09-20)
Sprawdzone bez wgrywania (`esptool flash_id`), bo obie płytki mają natywne USB:
- **COM9 = ESP32-S3, 16 MB flash** - master (`192.168.1.143`), build `Sep 20 2026`,
- **COM10 = ESP32-C3, 4 MB flash** - węzeł C3 SuperMini (adres 2, piny RX 20 / TX 21 / DE 10).

Wgrane (za zgodą użytkownika, **tylko aplikacja, NVS nietknięte**):
master przez `esptool write_flash 0x10000` (1 566 880 B, SHA-256 z nagłówka obrazu zgodny),
węzeł przez `pio run -e esp32c3-node -t upload --upload-port COM10`. Master wstał jako
`fw 1.0.0 (master)`, Wi-Fi/statyczne IP/ustawienia zostały (1557 B NVS, 52 klucze).

Naprawiony błąd: wgrany wcześniej master **nie obsługiwał argumentów** `/api/node`
(`GET /api/node?c=chan&addr=2&a=xyz` zwracał 200, `POST ...&c=pins_set` -> "brak sekcji 'a'").
Po wgraniu: `?a=xyz` -> `{"error":"Argumenty muszą być obiektem JSON"}`.

### ROZWIĄZANE (2026-09-20): magistrala RS485 - diagnostyka zabierała pady UART0

> **Sprostowanie (ważne).** Diagnoza "przewód z GPIO 38 zwarty do masy", opisana niżej, była
> **błędna**. Użytkownik miał rację, że to nie przewody i nie piny - **winny był kod diagnostyki
> w firmware**. Pomiary `idle: 0` na pinach 38 (master) i 20 (węzeł) były **artefaktem samego
> testu**: robił on `pinMode()` na padzie UART0, więc linia zostawała bez nadajnika i bez
> podciągu, a taki wiszący pin czyta się jako 0. Po poprawce ten sam pin 38 daje `idle: 100`,
> a magistrala pracuje bez błędów. Stary opis zostawiam tylko jako zapis fałszywej ścieżki.

Objawy: master `/api/rs485` pokazywał `online: false`, `rx: 0`, `err_timeout` rosnące,
`err: "brak odpowiedzi węzła"`, a węzeł na konsoli COM10 co 30 s powtarzał
`RS485: węzeł 2 - cisza na RX 20 (0 bajtów)`. Piny zgodne z mapą (master RX 39 / TX 38 /
DE 47, węzeł RX 20 / TX 21 / DE 10), `/api/pins` bez konfliktów, więc podejrzenie padło na
firmware - **niesłusznie**.

Dowody, w kolejności zdobywania (wnioski z punktów 1-2 były trafne, wniosek z punktu 3 był
**błędny**):
1. **Master jednak odbiera z węzła**: `/api/rs485` pokazał `raw_rx: 389`, `raw_s: 87` oraz
   podgląd pierwszych bajtów `sniff: "45 53 50 2D 52 4F 4D 3A"`, czyli ASCII `ESP-ROM:` -
   banner bootloadera C3 wysłany na jego UART0 (GPIO 21). Czyli **przewód 21 -> 39, nadajnik
   węzła i odbiornik mastera są sprawne**; magistrala nie milczy "w obie strony".
2. `readResponse()` zgłasza "brak odpowiedzi węzła" tylko wtedy, gdy **nie dotarł ani jeden
   bajt** (przy śmieciach byłoby "przerwana odpowiedź węzła"), a `frameFeed()` liczy
   `err_crc`/`err_frame` dopiero po rozpoznaniu nagłówka `AA 55`. Skoro węzeł raportuje
   `rawRx == 0`, to na jego RX **nie dotarł ani jeden bajt** - a luźny przewód zbiera szum
   i co jakiś czas dałby losowy bajt. Tak zachowuje się tylko linia trzymana w stanie LOW:
   każdy bajt kończy się naruszeniem bitu stopu i jest wyrzucany.
3. Nowa diagnostyka potwierdziła to pomiarem na żywo: **GPIO 38 ma poziom jałowy 0 mimo
   włączonego podciągu** (~45 kΩ przegrywa ze zwarciem), a wszystkie pozostałe piny
   (39, 47, 20, 21, 40-48, 0-18) mają 100, czyli są wolne.
   -> **BŁĄD W ROZUMOWANIU** (patrz lista przyczyn niżej, punkt 3): tak zmierzony pin 38 zostawał
   po teście bez podciągu, więc jego odczyt był fałszywy. Prawdziwym pomiarem jest `drive: 50`
   z pinu 38.

**Prawdziwa przyczyna - kod diagnostyki w firmware, nie przewody.** Diagnostyka "cisza na linii"
odbierała pady UART0 obu płytkom:
1. **Master**: `diagIdleLevel()` (wołane przez `?a=sniff`) robiło `pinMode(pin, INPUT_PULLUP)`,
   a na końcu `pinMode(pin, OUTPUT)` + HIGH + `pinMode(pin, INPUT)`. Dla pinu 38 (TX magistrali)
   `pinMode()` **odbiera pad nadajnikowi UART0** - po teście master przestawał cokolwiek wypuszczać
   na przewód aż do restartu. `pinMode()` na pinie DE (47) zostawiał go jako zwykłe wejście, więc
   "pomiar DE" (`idle_de: 0`) też był bez sensu.
2. **Węzeł**: jego własny log ciszy (`nodeLineLevel()`, rola headless) robił to samo na
   **GPIO 20, czyli na własnym pinie RX UART0** - węzeł przestawał widzieć cokolwiek do restartu.
   Stąd `rawRx == 0` u węzła przy jednocześnie rosnącym `tx` u mastera.
3. Gdy obie strony straciły swoje pady, przewód wisiał jako wolny pin bez nadajnika i bez
   podciągu - a taki pin czyta się jako **0**. To dokładnie te pomiary, które uznano za "twarde
   zwarcie do masy". Po poprawce ten sam pin 38 daje `idle: 100`, więc zwarcia nie ma.
4. `scoutWire()` przechodził 24 piny, zostawiał każdego kandydata jako `pinMode(p, INPUT)`
   (dla wolnych pinów nieszkodliwe, ale piny magistrali i DE trzeba było przywracać portem),
   a cały przegląd blokował obsługę strony (AsyncWebServer) na ~19 s - dlatego `?a=scout`
   "wisiał" 90 s i urywał połączenie z przeglądarką.

**Poprawki w kodzie** (`src/rs485.cpp`, `src/rs485.h`, `src/web_server.cpp`):
- `diagIdleLevel()` używa `gpio_set_pull_mode()` (`PULLUP_ONLY` -> próbki -> `FLOATING`), czyli
  zmienia tylko podciąg, a nigdy kierunek ani funkcję padu.
- `nodeLineLevel()` w węźle: `gpio_set_pull_mode()` + próba z `PULLDOWN_ONLY`; bez `pinMode()`.
- `sniffTx()` na koniec robi `stopPort(); startPort();` - port magistrali powstaje od nowa,
  więc pady wracają do UART0 (wcześniej zostawały odłączone).
- `scoutWire()`: nowy filtr `diagPinBusy()` (pomija 19/20 = USB oraz piny zajęte przez I2C, SD,
  GPS, DHT, AS3935), zawsze pomija pin DE, a cały przegląd ma budżet `RS485_SCOUT_BUDGET_MS`
  (3 s). Pusta lista pinów kończy się natychmiast, z zwolnieniem mutexa.
- Nowy `Rs485Bus::restartPort()` + `GET /api/rs485/action?a=restart` - restartuje sam port
  magistrali (bez restartu stacji). To on przywracał łączność po testach.
- `?a=sniff` jest teraz uczciwe: dla pinu TX mastera mierzy poziom jałowy (`idle`) i nadaje
  **na żywo** (bajty 0xAA próbkowane w trakcie nadawania -> `drive` = % próbek w LOW; sprawny
  nadajnik daje ~50 %, 0 % = pad nie nadaje). Pin RX mastera jest tylko **nasłuchiwany**
  (`listen: true`, bez nadawania - wcześniejsze nadawanie po linii węzła psuło odbierane ramki
  i dorzucało `err_crc`), a jego sprawność pokazują liczniki działającej wymiany
  (`link_raw_rx`, `link_rx`, `link_ok`). `idle_de` (pomiar podciągiem na pinie wyjściowym,
  zawsze 0) zastąpione przez `de_level` = `digitalRead(pinDe_)`.

Węzeł ma w logu własny pomiar linii (tylko rola headless, `nodeBusDiag()`), teraz bez
`pinMode()` - podciąg jest ustawiany przez `gpio_set_pull_mode()`, więc węzeł nie odbiera sobie
własnego pinu RX:
```
RS485: węzeł 2 - cisza na RX 20 (0 bajtów), poziom linii RX: <0-100>/100 (0 = linia w GND,
100 = wolna), ktoś nią steruje: <0|1>
```
Pierwsza liczba to 100 próbek `digitalRead()` przy podciągu do plusa (bez zmiany kierunku pinu!),
druga mówi, czy linia jest trzymana w stanie wysokim również przy podciągu do masy - czyli czy
ktoś nią aktywnie steruje.

#### Narzędzie: "lekarz magistrali" - `/api/rs485/action?a=sniff|scout|restart`
Dodane do `src/rs485.cpp` (`sniffTx()`, `scoutWire()`, `restartPort()`), działa tylko na masterze,
**nie rusza NVS** i nie zmienia sensu statystyk `tx`/`rx`. Na czas testu użycza UART1 od GPS
(`gps.end()`) i oddaje go (`gps.begin()`), więc działa też przy włączonym GPS/PMS bez sprzętu.
- `?a=sniff&pin=38&ms=400` (pin = własny TX mastera) - `idle` (poziom jałowy, 100 = linia wolna)
  oraz `drive` = % próbek w LOW w trakcie nadawania bajtów 0xAA. Zdrowa linia: `idle: 100`,
  `drive` ok. 50 %. `drive: 0` = pad nadajnika nie wypuszcza danych (np. zajęty przez inne
  peryferium), `idle: 0` = coś zewnętrznego trzyma przewód w GND.
- `?a=sniff&pin=39` (pin RX mastera) - nasłuch bierny: `listen: true`, `bytes`/`hex` (co przyszło
  z linii węzła) oraz `link_raw_rx`/`link_rx`/`link_ok` z działającej wymiany. W czasie testu
  master nie odpytuje węzła, więc cicha linia w tym oknie jest normalna - o sprawności pinu RX
  mówią liczniki `link_*`.
- `?a=sniff&pin=<inny>` - nadaje prawdziwą ramkę `ping` po tym pinie i słucha odpowiedzi.
- `?a=scout&pins=38,47,21&ms=150` (bez `pins` = piny wolne, piny zajęte i DE pomijane przez
  `diagPinBusy()`, cały przegląd w budżecie 3 s) - dla każdego pinu: `idle`, `sent`, `bytes`,
  `hex`, `reply`; `listen: true` = pin RX mastera (tylko pomiar), `found` = pin, z którego
  węzeł odpowiedział, `idle: 0` = coś trzyma linię w GND.
- `?a=restart` - zamyka i otwiera UART0 od nowa (nadajnik, odbiornik i DE wracają na miejsce)
  oraz wymusza natychmiastowe odpytywanie węzła. Używać po diagnostyce, jeśli łączność nie
  wróciłaby sama.

Testy wykonane na sprzęcie (2026-09-20, po wgraniu nowego mastera przez OTA i węzła przez COM10):
1. Przed poprawką: `?a=sniff&pin=38&ms=400` -> `{"idle":0,...}` (i ten sam pomiar w węźle na
   GPIO 20) - to był artefakt diagnostyki, nie zwarcie.
2. Po poprawce: `?a=sniff&pin=38&ms=400` -> `{"idle":100,"de_level":0,"self_tx":true,"drive":50}`
   - **pad TX mastera naprawdę nadaje w przewód** (50 % próbek w LOW), a linia nie jest zwarta.
3. `?a=sniff&pin=39&ms=3000` -> `listen: true`, `sent: 0`; sprawność pinu RX potwierdzają
   liczniki `link_raw_rx` (dziesiątki kB) i podglądane ramki `sniff`.
4. `?a=restart` -> `ok: true` i natychmiast `online: true`, `latency_ms` 167-199 ms.
5. Magistrala stabilna: `tx 10 / ok 10 / err_crc 0 / err_timeout 0`, po 75 s kolejne
   `tx +11 / ok +10 / err_timeout +0`. Późniejsze kontrole (te same wersje firmware):
   `tx 17 / rx 15 / ok 15 / err_crc 2 / err_timeout 2`, `raw_rx 47695`, `latency_ms 195`,
   `drive 52`, `mirrored 169 / meta 179`. Te 2 błędy to wyłącznie odpytywania ze startu
   (`reload()` ustawia `nextPollMs_ = 0`, więc master próbuje ~1 s po starcie, gdy węzeł jeszcze
   się nie podniósł) - później licznik `err_timeout` już nie rośnie.
6. Węzeł `addr 2`, `fw 1.0.0 (node)`, `build Sep 20 2026 13:19:04`, `mac 7C:4D:37:D4:DB:1C`,
   `heap 174368`, `channels 179`, `board c3mini`, `chip ESP32-C3`, `flash_mb 4`, `sd: false`.
7. Regresja: po `?a=sniff` i `?a=scout` łączność **nie** wymaga już restartu stacji - port
   wraca przez `stopPort()/startPort()`; `?a=restart` to tylko wygoda.

Po sprawdzeniu zostaje (nie blokuje pracy magistrali):
1. Paging kanałów węzła (`warnNoPaging()`): `?c=chan&addr=2&a={"o":33,"n":3}` ma dać `off: 33`,
   3 kanały, potem pełne `?c=chan&addr=2` -> 179 kanałów i odzwierciedlone `x2_*`.
2. Przycisk "Restart portu" w zakładce RS485 (endpoint `?a=restart` już działa, obecnie wołany
   z paska adresu) - wymaga zbudowania i wgrania obrazu LittleFS.
3. Kolejne węzły C3 SuperMini (deszcz, gleba, woda w szafce) z gotowymi binarkami i adresami
   3, 4, 5... plus osobne podmenu "ESP #2/#3/..." na stronie - patrz "Do rozważenia".

## Blokery

Brak otwartych blokerów - OTA (firmware, system plików www i espota) działa i jest sprawdzone
na sprzęcie (patrz sekcja "Aktualizacja OTA" wyżej).

## Do zrobienia

### Węzeł C3 SuperMini - nowe domyślne piny (magistrala działa, 2026-09-20)
- Nowe domyślne piny węzła (`src/board.cpp`, `C3_PINS[]`): I²C SDA 8 / SCL 9 (AS5600),
  anemometr (kontaktron/hall) 5, deszcz -1, wiatrowskaz -1, LED -1, przycisk -1,
  DS18B20 4, ADC gleby 1, RGB 2, RS485 RX 20 / TX 21 / DE 10.
- Przesłanie tych pinów do węzła przez magistralę: `POST /api/node?c=pins_reset&addr=2`
  (albo `pins_set` z jawną mapą) i kontrola `GET /api/node?c=pins&addr=2`.
- Przebudowanie binarek węzła w wydaniu - obecne `data/node/c3mini*.bin` powstały jeszcze
  na starych domyślnych pinach: `tools/build_release.ps1`.

### Sterowniki - dostarczone, czekają na weryfikację na sprzęcie
- `src/drv_gas.*` - **nieprzetestowane**: krzywe MQ/MiCS/TGS (a, b) i typowe R0 są przybliżone
  (fit z not katalogowych), ścieżka ADS1115 skompilowana, ale nigdy nie uruchomiona.
  Czas wygrzewania MQ: 60 s od startu.
- `src/drv_tc.*` - SPI bit-bang: przed podłączeniem sprawdzić czasy (bit-bang dzieli
  magistralę z kartą SD tylko programowo, ale piny są osobne).
- `src/drv_motion.*` - konflikty adresów I²C udokumentowane w kodzie (0x68 RTC, 0x29 światło,
  0x5A CCS811, 0x18/0x19/0x1D MCP9808) - wykrywanie jest zachowawcze.
- `src/drv_i2s.*` - 3 bloki × 256 ramek na cykl pomiarowy (~64 ms), więc LEQ i szczyt są
  wartościami orientacyjnymi; na ESP32-C3 jest tylko jeden kontroler I²S (RX i TX naraz się nie da).
- **MCP3008/MCP3208** ma od teraz **własną linię `adc_cs`** (rola w edytorze pinów) - wcześniej
  dzielił `tc_cs` z termoparami, więc nie dało się użyć termopary i przetwornika naraz.
  Wiersz katalogu dopisany (`docs/CZUJNIKI.md`, sekcja 34).
- Po wgraniu firmware: sprawdzić na stronie nowe kanały (`tc_*`, `mot_*`, `snd_*`, `gas_*`),
  nowe pola w "Zaawansowana kalibracja układów" i że zapis do NVS nie kasuje innych kluczy.
- **Znane ograniczenie (sprzed zmian, nie regresja)**: `GET /api/node?c=status&addr=<a>`
  zwraca `{"ok":false,"err":"odpowiedź za duża (limit 8192 B)"}` - odpowiedź węzła nie mieści
  się w ramce RS485 (limit `max_payload` = 8192 B). Strona tego nie używa (korzysta z
  `sync`, `chan_set`, `pins`, `pins_set`, `cfg`, `cfg_set`, `log`, `diag`). Do decyzji:
  albo podnieść limit, albo zawęzić odpowiedź `status` węzła.

### Budowy i wdrożenie
- Build `esp32s3-master` + `esp32c3-node`: **SUCCESS** (S3 po naprawie `STACJA_HEADLESS`:
  flash **49,3 %** / 1550041 B z 3145728 B, RAM 22,1 % / 72532 B;
  C3 headless: flash **41,7 %** / 710190 B z 1703936 B, RAM 8,7 % / 28568 B - ~994 kB zapasu).
  Uwaga: liczba 89,5 % z poprzedniej notatki dotyczyła starego `build_src_filter`
  (węzeł C3 budował wtedy część modułów sieciowych).
- Firmware mastera wgrane przez **USB (COM9, tylko 0x10000)** - NVS i LittleFS nietknięte
  (`fs_used` 491520 B, ustawienia zostały); `dist/` przebudowany na nowo.
- Węzeł C3 **nie był wgrywany** - dla `esp32c3-node` zmiana `#ifdef` -> `#if` jest
  równoważna (`STACJA_HEADLESS=1` z `-D`), więc binarka zachowuje się tak samo; wgranie
  nowej wersji na COM10 tylko za wyraźną zgodą.
- `esp32s3-node` (druga płytka S3 16 MB) i `esp32s3-node-8mb` (N8R8) - budowane w walidacji,
  ale **poza wydaniem**: `tools/build_release.ps1` składa teraz trzy binarki - `master-16mb`,
  `node-8mb-quad` (N8R4/N8R2 - zalecany węzeł) i `node-c3-4mb`. Obraz LittleFS 8 MB powstaje
  ze środowiska `esp32s3-node-8mb-quad` (ta sama tabela partycji co `esp32s3-node-8mb`).
- Profile `s3n16r8` i `s3n8r8` w `src/board.cpp` mają `buildable = false` (brak binarki
  w wydaniu) i opis, jak je zbudować ręcznie; profile `s3n8r4` i `c3mini` wydają pliki
  z zakładki **ESP i magistrala RS485 → Pliki do wgrania węzła**.
- LittleFS + firmware OTA na 192.168.1.143 - **sprawdzone**: firmware 1,5 MB ~13 s,
  LittleFS 9,94 MB 72 s, po wgraniu `/` zwraca `index.html` (220816 B), `fs_used` 487424 B,
  a `POST /api/reboot` odpowiada po przerwaniu wgrania. Do zrobienia zostaje tylko przejście
  tego samego z przeglądarki (zakładka *Aktualizacja*) i kontrola `/api/status`, `/api/sensors`,
  `/api/node?c=chan` oraz sekcji "Zaawansowana kalibracja układów" na stronie.
- `data/legend.js` (**99 ok / 126 plan**, 225 wierszy) został przegenerowany - musi zostać
  wgrany razem z LittleFS.
- **Naprawiony błąd wydania**: obraz `-www-8mb.bin` powstawał z pełnego `data\`, więc binarki
  węzłów (4,6 MB) nie mieściły się w 1,88 MB partycji LittleFS płytki 8 MB. `mklittlefs`
  kończył pracę kodem **0**, a obraz wychodził z plikami 0 B (`lfs_write error(-28)` widoczne
  tylko w logu) - taki plik dawał pustą stronę po wgraniu. Teraz:
  - `tools/stage_node_fs.py` (PlatformIO `extra_scripts = pre:...` w `esp32s3-node*` i
    `esp32c3-node`) podstawia `PROJECT_DATA_DIR` na kopię `data\` **bez** katalogu `node`
    (`%TEMP%\stacja-www-<env>`), więc `pio run -e <węzeł> -t buildfs/uploadfs` też jest bezpieczne;
  - `tools/build_release.ps1` buduje oba obrazy przez `pio run ... -t buildfs`, a potem
    **sprawdza** rozmiar (równy partycji), obecność `/index.html`, brak wpisu `0 B`,
    obecność `/node/` w obrazie 16 MB i brak `/node/` w obrazie 8 MB - błąd przerywa wydanie;
  - kolejność jest ustalona: najpierw sklejanie i `-PublishNode`, potem obrazy LittleFS,
    więc `-www-16mb.bin` ma świeże binarki węzłów.
  Sprawdzone: `dist\` przebudowany (exit 0, brak `lfs_write`), `/index.html` 235218 B i wszystkie
  pliki mają zgodne MD5 z `data\`, `-www-8mb.bin` = 1 920 kB bez `/node/`, `-www-16mb.bin`
  = 10 176 kB z czterema świeżymi binarkami. Poprawione też nieaktualne rozmiary partycji
  4 MB w README (spiffs `0x350000` / 704 kB, sloty `0x1A0000` / 1,63 MB) i komentarze
  w `partitions-8mb.csv` / `partitions-4mb.csv`.
- **Sprawdzone na żywo po wgraniu mastera (COM9, app 0x10000 + LittleFS)**: `/api/status`
  zwraca `fw 1.0.0 (master)`, `board S3 N16R8`, `psram_mb 7`; `/`, `/i18n.js`, `/legend.js`,
  `/public.html`, `/LICENSE.txt` mają MD5 zgodne z `data\`; `/api/rs485` pokazuje
  `nodes[addr=2 online board=c3mini ch=179 err=0]`, `active_addr=2`, `node_count=1`.
  Strona (SPA) w przeglądarce: w menu bocznym **Grupa „ESP i magistrala RS485"** ma pozycję
  **ESP #2 • 2** (zielona, dopisuje się po odczycie stanu), zakładka pokazuje tabelę węzłów
  (`ESP #2 • 2 online, C3 SuperMini, ESP32-C3, 7C:4D:37:D4:DB:1C, 179 kanałów`), przyciski
  *Dodaj węzeł / Zmień adres węzła / Skan magistrali / Szukaj węzłów / Odczytaj dane węzła*,
  tabelę pinów magistrali (RX 39, TX 38, DE 47), listę 37 odzwierciedlonych kanałów `x2_*`
  oraz sekcję **Pliki do wgrania** - po wybraniu `s3n8r4` pojawiają się linki
  `/node/s3n8r4.bin` i `/node/s3n8r4-ota.bin`. Pobrane z mastera pliki są **bajt w bajt**
  identyczne z `data\node\*` (c3mini 853456 B, c3mini-ota 787920 B, s3n8r4 1630512 B,
  s3n8r4-ota 1564976 B, MD5 zgodne). `/api/rs485/action?a=add&addr=3` i `?a=del&addr=3`
  działają (lista węzłów rośnie do 2 i wraca do 1, węzeł 2 bez zmian). Przełącznik języka
  zmienia interfejs (EN -> PL: *Dashboard -> Pulpit*, *Local forecast -> Prognoza lokalna*).
- **Rozpoznanie portów COM (bez wgrywania, 20.09.2026)**: laptop ma dwa urządzenia Espressif
  (VID 303A, PID 1001): **COM9 = master (ESP32-S3)**, **COM10 = węzeł (ESP32-C3 SuperMini)**.
  Potwierdzone dwiema niezależnymi drogami: `arp -a 192.168.1.143` daje MAC `80-B5-4E-C6-52-A8`,
  czyli MAC rodzica (urządzenia kompozytowego USB) portu COM9; a numer seryjny USB portu COM10
  (`1C:DB:D4:37:4D:7C`) to odwrócony bajtowo MAC węzła raportowany po RS485 (`7C:4D:37:D4:DB:1C`).
  Wniosek na przyszłość: wystarczy porównać MAC z ARP (master) albo MAC węzła z `/api/rs485`
  (odwrócony) - nie trzeba podłączać esptool do portu, żeby sprawdzić, gdzie jest która płytka.
- **Magistrala RS485 z węzłem C3 - sprawdzona na żywo po podłączeniu pinów 38/39**:
  `/api/rs485` zwraca `online: true`, `board: c3mini`, `chip: ESP32-C3`, 179 kanałów,
  `mirrored: 37`, `latency_ms: 203`, a licznik ramek `tx 109 / rx 105 / ok 89`,
  `err_crc 1`, `err_timeout 21`, `retries 19` (sporadyczne powtórzenia - magistrala bez
  terminacji, przewody na pająka). Węzeł raportuje `heap 177044`, `boot 319`, `warns 13`.
- **Nowe domyślne piny węzła C3 (ESP #2 - wiatr)**: w `src/board.cpp` szablon `C3_PINS` to teraz
  `i2c_sda 8`, `i2c_scl 9` (AS5600 0x36), `anem 5` (kontaktron/Hall), `rain -1`, `vane -1`,
  `led -1`, `button -1` (GPIO 8/9 zajmuje I2C), `onewire 4`, `soil_adc 1`, `rgb 2`,
  RS485 `rx 20 / tx 21 / de 10`. `NOTE_C3` i podpowiedzi w `PADS_C3` opisują ten zestaw,
  a `docs/CZUJNIKI.md` (blok "Piny domyślne węzła ESP32-C3 SuperMini") i README (rozdział
  RS485) mają legendę. Obie binarki zbudowane po zmianie: `esp32s3-master` OK
  (Flash 1566513 B, 49,8 %), `esp32c3-node` OK (Flash 723328 B, 42,5 %) - **nic nie zostało
  wgrane**. Węzeł na żywo ma jeszcze stare piny (sda 6, scl 7, anem 3, rain 5, vane 0,
  led 8, button 9), bo nowe wchodzą do firmware przy następnym wgraniu albo przez
  `POST /api/node?c=pins_set&addr=2` (SPA sama restartuje węzeł po zapisie).
- **Znalezione (20.09.2026): wgrane firmware mastera nie ma jeszcze obsługi argumentów
  w `/api/node`.** W źródle `src/web_server.cpp` (nodeProxy) argumenty polecenia bierze się
  z treści żądania (`bodyOf`) albo z parametru `?a=`, i odrzuca się `?a=` nie będące obiektem
  JSON - ale stacja na 192.168.1.143 odpowiada `200` na `?c=chan&addr=2&a=xyz`, a
  `POST /api/node?c=pins_set&addr=2` z poprawną treścią JSON kończy się `brak sekcji 'a'`
  (węzeł dostaje ramkę bez `a`). Czyli na urządzeniu jest build **starszy** od tego drzewa:
  ma już blokadę "To polecenie wymaga metody POST" (400 na GET), ale nie ma jeszcze
  przekazywania argumentów. Skutek: z www nie da się **zapisać** niczego w węźle
  (`pins_set`, `cfg_set`, `chan_set`, `time`, `action`), a `?c=chan` zawsze zwraca pierwszą
  stronę 12 kanałów. Poprawka jest w źródle - wystarczy wgrać świeżo zbudowany
  `esp32s3-master` (app od `0x10000`, NVS nietknięte).

### Do rozważenia
- ESP32-C2 / ESP8684 jako trzeci typ węzła - **odradzane**: nowe środowisko, tabela partycji,
  profil płytki i kolejna binarka w wydaniu, a układ nie ma PSRAM ani I2S (moduł audio
  `drv_i2s` byłby bezużyteczny). Do węzła wystarczą dwie rodziny: **S3 (N16R8/N8R4)**
  i **C3 SuperMini**; ESP32-C6 zostaje jako szablon pinów bez binarki.
- Radar (HLK-LD2410/LD2410B) - rola pinu jest, kanału jeszcze nie ma.
- Wyświetlacze (SSD1306/SH1106, LCD, TFT, e-ink), kamera OV2640/5640, RC522/PN532,
  serwa/PWM, przekaźniki, buzzery - obecnie nieobsługiwane (statusy w katalogu są zgodne
  z rzeczywistością).
- Współdzielenie odczytów (temperatura, PM2.5) na stronie z reklamą firmy w adresie.
