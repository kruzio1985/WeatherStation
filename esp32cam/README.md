# Zewnętrzna kamera ESP32-CAM

Osobny moduł kamery dla projektu **Stacja Pogody**. Moduł (ESP32-CAM AI-Thinker
z OV2640) robi zdjęcia JPEG i wysyła je HTTP POST do stacji-mastera pod adres:

```
http://<adres-mastera>/api/camera/upload
```

Master odbiera zdjęcie, zapisuje je na karcie SD (`/photos/YYYY-MM/`), analizuje
(jasność, luksy, dzień/noc, RGB, ruch, zachmurzenie) i pilnuje limitów galerii.

## Wgranie firmware

```powershell
cd C:\Projects\StacjaPogody\esp32cam
pio run -t upload --upload-port COM<port kamery>
```

Skrypt [tools/build_release.ps1](../tools/build_release.ps1) buduje też gotowe
pliki do wgrania w katalogu `dist/`:

- `StacjaPogody-<wersja>-cam-4mb.bin` — pełny obraz od adresu `0x0`
  (bootloader + tablica partycji + firmware), na nową płytkę.
- `StacjaPogody-<wersja>-cam-4mb-ota.bin` — sam firmware (do aktualizacji).

## Konfiguracja

Domyślne ustawienia są w [src/config.h](src/config.h). Można je też zmienić
bez ponownej kompilacji:

1. **Strona www kamery** — otwórz `http://<ip-kamery>/` i wypełnij formularz
   (Wi-Fi SSID, hasło, adres mastera), albo
2. **Monitor szeregowy (115200)** — wpisz komendę:

```
SET SGC haslo http://192.168.1.143
```

Ustawienia zapisują się w NVS kamery i są używane po restarcie.

## Aktualizacja OTA

Kamera ma włączony ArduinoOTA (port 3232, hasło z [src/config.h](src/config.h):
`CAM_OTA_PASS`). Aktualizację można wgrać z PlatformIO:

```powershell
pio run -t upload --upload-port <ip-kamery> --upload-protocol espota --auth <hasło>
```

albo przez stronę www stacji, jeśli dodasz tam kamerę jako urządzenie OTA.

## Jak to działa

- `GET /capture` — robi zdjęcie, wysyła je do mastera i zwraca JPEG do
  przeglądarki. To ten endpoint ustawiasz na stacji w polu
  **„Adres kamery zewnętrznej"** (np. `http://192.168.1.50/capture`).
- `GET /status` — JSON ze stanem kamery.
- `GET /` — prosta strona konfiguracyjna.
- Timelapse realizuje master: co `cam_interval_min` minut wywołuje `/capture`
  na kamerze, a zdjęcie wraca samo przez `/api/camera/upload`.
