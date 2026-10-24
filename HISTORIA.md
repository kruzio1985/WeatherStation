# Stacja Pogody — historia projektu (master)

> Ten plik jest pamięcią projektu. **Przeczytaj go na starcie nowego czatu.**
> Zasady pracy i lista zakazanych (drogich) operacji: `.github/copilot-instructions.md`.

## Czym to jest

Stacja meteorologiczna na **ESP32-S3 WROOM-1 N16R8** (16 MB flash, 8 MB PSRAM octal).
Master zbiera dane z własnych czujników i z węzłów (RS485 + Wi-Fi), publikuje do
Home Assistant przez MQTT, prowadzi logi na karcie SD, rysuje ekran TFT i wystawia
panel www.

- **Adres:** `192.168.1.143` (statyczny, SSID `SGC`)
- **Hostname:** `stacja-pogody` (mDNS działa nieregularnie — używaj IP)
- **Hasło OTA / akcje niszczące:** nagłówek `X-OTA-Key`
- **Hasło MQTT:** w `OfflineWorkspace\HISTORIA\SEKRETY\mqtt pogoda.txt` (nie commitować)

## Sprzęt i piny (stan aktualny)

| Element | Piny / adres |
|---|---|
| Wyświetlacz TFT | **ST7789** 240×320 (GMT024-10), SCK=40, MOSI=7, CS=21, DC=41, RST=3, obrót 90° |
| Pierścień LED RGB | **wyłączony** (−1) — GPIO 7 oddane wyświetlaczowi |
| Tuya (temp/wilg.) | **wyłączony** (−1) — GPIO 40 oddane wyświetlaczowi |
| Karta SD (SPI) | CS=12, SCK=10, MOSI=11, MISO=13 |
| I²C | SDA=8, SCL=9 |
| RS485 (master) | RX=39, TX=38, DE=45 |
| Przycisk (BOOT) | GPIO 0 |
| Deszcz / wiatr / wiatrowskaz | 5 / 6 / 1 |
| UV / liść | 4 / 2 |

## Węzły (skąd master bierze dane)

| Węzeł | Adres | Typ (ExtDev) | Kanały |
|---|---|---|---|
| Stacja powietrza wewnątrz | 192.168.1.141 | `aq` | `in_*` (25) |
| Stacja powietrza zewnątrz | 192.168.1.147 | `outaq` | `c3_*` (11) |
| VEVOR 868 MHz | 192.168.1.130 | `vevor` | `vev_*` |
| Kamera ESP32-CAM | 192.168.1.142 | — (przez `/api/camera/upload`) | `cam_*` |
| Broker MQTT (HA) | 192.168.1.14:1883 | — | prefix `stacja_pogody` |

Żywotność węzłów bierzemy **z tego, co same przysyłają** (`uptime_s`, `mqtt` w ich
`/api/status` + chwila odebrania odpowiedzi) — **żadnych adresów IP na sztywno**.

## Struktura

- `src/` — firmware (`main.cpp`, `web_server.cpp`, `api_json.cpp`, `drv_display.cpp`,
  `sensors*.cpp`, `extdev.cpp`, `mqtt_client.cpp`, `ota.cpp`, `loop_stats.cpp`, ...)
- `include/` — `pins.h`, `config.h`
- `data/` — strona www: `index.html` (SPA, ~396 kB), `i18n.js`, `legend.js`, `favicon.svg`
- `partitions*.csv` — tabele partycji (16 MB / 8 MB / 4 MB)
- `blegw/`, `esp32cam/` — pokrewne projekty węzłów (osobne buildy)
- `tools/`, `docs/`

> Projekt **stacji powietrza zewnętrznego został rozdzielony** do osobnego katalogu
> `PROJEKTY_W_BUDOWIE\Stacja Powietrza Zew` — nie ma go już w tym drzewie.

## Ekran TFT — co potrafi (stan aktualny)

- **Górny pasek:** trzy znaczniki stanu naraz — `KAMERA`, `C3` (powietrze zewn.),
  `WEW` (powietrze wewn.). Kropka stanu na końcu opisu, promień zależny od czcionki.
- **Środek:** rotacja wskazań co `disp_step_s` (5 s) po liście `disp_ch` (13 kanałów
  z różnych czujników). Duża liczba + jednostka tuż za nią + krótki opis.
- **Kolory temperatury:** <10 °C błękity → żółty → pomarańcz → czerwień.
- **Dolny wiersz:** zegar (lewo), licznik cyklu (prawo), stan sieci na pulpicie (środek).
- **Dolny pas:** 6 progów AQI (te same kolory hex, co legenda na www) + znacznik pomiaru.
- Rozbicie: `H` − pas; kropki cyklu usunięte (były nieczytelne).

### Edytor wyglądu w www
Zakładka **„Wyświetlacz TFT" → sekcja „Wygląd i pozycje"**: dla każdego elementu
(`top`, `lbl`, `val`, `unit`, `info`, `wx`) ustawiasz **pokaż / rozmiar czcionki
(16/26/48/75 px, 0 = auto) / X / Y / kolor**, plus które statusy pokazywać na górze,
pas jakości (wł./wysokość/kolor) i rozmiar grafiki pogody (%). Jest „Przywróć domyślne".
Zapis: `POST /api/display` z kluczem `ui`; w NVS jako `disp_ui_*`.

## Pułapki — czytaj przed każdą akcją

1. **Przycisk BOOT (GPIO 0) przytrzymany 3 s = RESET DO USTAWIEŃ FABRYCZNYCH.**
   Kasuje NVS: piny TFT na −1 (biały ekran), kamerę, MQTT, wszystko. **Nie dotykać
   przycisku przy restarcie urządzenia.**
2. **`uploadfs` przez espota potrafi przerwać się w połowie** i zostawić system
   plików bez strony (`GET /` nie odpowiada). Wgrywaj obraz FS przez
   `POST /api/ota/filesystem` (multipart + `X-OTA-Key`) — ta droga jest pewna.
3. **Nie używać** `POST /api/factoryreset` ani `POST /api/sd/format` bez wyraźnej zgody.
4. **Nie ruszać Wi-Fi laptopa** użytkownika.
5. LCD logiczny **320×240** (po obrocie), a nie 240×320 — układy dobierają się do
   `width()`/`height()` po `setRotation()`.
6. Czcionka 7-segmentowa (Font7) **nie ma przecinka** — przy „17,2" kod sam bierze Font8.
7. Nazwy kanałów z węzłów bywają w obcym języku (np. włoskie „Umidità") — czcionki
   ekranu ich nie mają, funkcja `plSafe()` sprowadza je do liter bazowych i pomija resztę.
8. Buduj **dwa razy** (oba SUCCESS) i przejrzyj `git diff --stat` przed wgraniem.
9. Wgrywanie firmware: `POST /api/ota/firmware` (multipart, `X-OTA-Key`), nie espota.

## Historia zmian (skrót)

- `d34da2a` ekran ST7789 (LovyanGFX), diagnostyka I²C
- `5991bd6` import powietrza zewnętrznego z C3 (`c3_*`), poprawka Wi-Fi przy pustym SSID
- `cb1bc8c` naprawa zawieszania na ExtDev (własne gniazdo), punkt rosy, polskie znaki
- `5417df0` TFT: trzy statusy naraz, rotacja 13 kanałów, kolory temperatury, pas AQI;
  przeniesienie poprawek P0/P1 (reset dobowy opadu, merge offsetów, ochrona `millis()`,
  JSON 404 dla nieznanych `/api`, timeout i bufor MQTT, `loop_stats`, heartbeat BOOT)
- `2d10919` nowy wygląd www (grafity, bez emoji — ikony SVG, tabele, formularze)
  + kamera jako czwarty węzeł sieci w zakładce „Węzły Wi-Fi"
- `7e6e649` TFT: duże wskazania na środku, krótkie opisy bez „?", usunięte kropki cyklu
- `6a94d95` panel „Wygląd i pozycje" (rozmiar/pozycja/kolor/widoczność elementów ekranu)
- favicon.svg + `meta description` + `theme-color` w nagłówku strony
- `9726644` Kamera: zdjęcie z modułu zapisywane w pętli głównej — koniec
  restartów stacji co ~48 s (`watchdog zadania`) i migania ekranu
- `f2885ca` Diagnostyka restartów: ślad etapu obiegu i ostatniego żądania www
  w pamięci RTC (przeżywa reset), podsumowanie analizy co 5 min zamiast co 1 min,
  limit czekania na kartę SD i cache `usedBytes()`

## Stan obecny (24.10.2026)

- **Karta SD jest bardzo wolna:** zapis 16 KB trwa **3,8 s** (powinno <50 ms).
  To źródło zatorów: obieg pętli 8 s przy każdym zdjęciu (co 5 min).
  Do rozważenia wymiana karty.
- Restarty z watchdoga: ślad w RTC wskazał etap `www/MQTT/OTA` tuż po
  `/api/status`; po naprawach (cache `usedBytes`, limit czekania na SD,
  tańsze podsumowanie analizy) brak restartów w obserwacji — następny
  nieoczekiwany restart wypisze w logu `Przed resetem: etap …, ostatnie żądanie: …`.
- Obieg pętli przy starcie: 4-6 s (pierwsze podsumowanie analizy), potem spokój.
- **Restarty z watchdoga — pierwsza naprawa** (`9726644`): uptime > 100 s
  (wcześniej nigdy > 48 s). Przyczyna: `POST /api/camera/upload` robił zapis
  na SD + dekodowanie JPEG + MQTT w zadaniu serwera www, które `AsyncTCP`
  rejestruje w watchdogu zadania (5 s, panic). Teraz bufor przechodzi do
  pętli głównej (`queueUpload()` → `processPendingUpload()`).
- Strona: `GET /` ≈ 397 kB, `i18n.js` ≈ 205 kB
- MQTT połączony (`192.168.1.14:1883`, user `StacjaP`), węzły 3/3 OK, SD zamontowana
- Dodany **przycisk restartu** (↻) w nagłówku strony → `POST /api/reboot`
- Naprawione: migające checkboxy w edytorze ekranu (odpytywanie w tle nadpisywało
  formularz — teraz w tle odświeża się tylko tabela „Stan")

## Do zrobienia

- Uzupełnić brakujące wpisy i18n dla nowych napisów edytora wyglądu
- Zbadać, dlaczego pierwsze wejście na zakładkę bywa wolne (duży `index.html` + `i18n.js`)
- Rozważyć przeniesienie `i18n.js`/`legend.js` na kartę SD
