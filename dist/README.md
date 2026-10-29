# Gotowe pliki do wgrania — Stacja Pogody 1.0.0 (ESP32-S3 WROOM-1 N16R8)

Zbudowane z commita `8048372` (2026-10-30). Firmware sprawdzony na stacji:
przez 5 dni pracy bez restartu.

| Plik | Rozmiar | Do czego |
|---|---|---|
| `StacjaPogody-1.0.0-master-16mb-ota.bin` | 1 927 280 B | **Aktualizacja działającej stacji** — strona www → zakładka *Aktualizacja* (albo `POST /api/ota/firmware` z `X-OTA-Key`). **Nie kasuje ustawień** (Wi-Fi, MQTT, kalibracje, piny zostają w NVS). |
| `StacjaPogody-1.0.0-master-16mb.bin` | 1 992 816 B | **Nowa płytka / po wyczyszczeniu flasha** — pełny obraz od `0x0` (bootloader + tablica partycji + firmware). Wgrywany od `0x0` **kasuje ustawienia**. |

## Wgranie przez stronę www (zalecane)

1. Otwórz `http://<ip-stacji>/` → zakładka **Aktualizacja**.
2. Wybierz plik `...-ota.bin`, podaj hasło OTA (nagłówek `X-OTA-Key`).
3. Stacja zrestartuje się sama po zapisaniu (~20 s).

Albo z konsoli (multipart, bez utraty ustawień):

```powershell
curl.exe -X POST "http://<ip-stacji>/api/ota/firmware" `
  -H "X-OTA-Key: <hasło OTA>" `
  -F "firmware=@StacjaPogody-1.0.0-master-16mb-ota.bin;type=application/octet-stream"
```

## Wgranie przez USB (nowa płytka)

```powershell
esptool --chip esp32s3 --port COMx write_flash 0x0 StacjaPogody-1.0.0-master-16mb.bin
```

## Uwagi

- Partycje: `partitions.csv` (16 MB, dwa sloty OTA po 3 MB + LittleFS ~9,94 MB).
  Wariant `-ota.bin` **nie dotyka** LittleFS ani NVS.
- **W pliku nie ma żadnych danych prywatnych** — sprawdzone przeszukaniem binarnego
  pliku: brak SSID/hasła Wi-Fi, brak adresów IP, brak danych brokera MQTT.
  Konfiguracja siedzi wyłącznie w NVS urządzenia.
- Domyślne hasło OTA w firmware to `stacja-ota` — **zmień je** w zakładce
  *Aktualizacja* przed wystawieniem stacji do internetu.
- Pełna strona www (`data/`) jest w LittleFS; jeśli po aktualizacji nie ma strony,
  wgraj system plików przez `POST /api/ota/filesystem` (nie przez `uploadfs`).
