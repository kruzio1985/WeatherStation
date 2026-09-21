# 🌤️ Weather Station — ESP32-S3 WROOM-1 N16R8

> 🇵🇱 [Polska wersja](#polski) &nbsp;·&nbsp; 🇬🇧 [English version](#english)
>
> *Autor / Author: **kruzio1985** — [github.com/kruzio1985](https://github.com/kruzio1985)*
> *Licencja / Licence: [PolyForm Noncommercial License 1.0.0](LICENSE)*

Kompletna stacja pogodowa z interfejsem WWW, wysyłką do Home Assistant przez MQTT
(auto-discovery), logowaniem na karcie microSD i wykresami (dni/tygodnie/miesiące/lata).

> **Stan projektu:** czujniki nie są jeszcze zlutowane. Kod jest gotowy do wgrania —
> każdy czujnik jest **opcjonalny** i wykrywany przy starcie. Brak czujnika nie powoduje
> błędu, po prostu nie pojawia się na stronie ani w MQTT.

<a id="english"></a>

## 🇬🇧 English — full description

A complete weather station with a web interface, MQTT publishing to Home Assistant
(auto-discovery), logging to a microSD card and charts (day/week/month/year).

> **Project status:** the sensors are not soldered yet. The code is ready to flash — every sensor
> is **optional** and detected at boot. A missing sensor does not cause an error; it simply does
> not appear on the page or in MQTT.

### Features

- **Side menu with submenus** — navigation is scrollable on the left and grouped:
  **Forecast** (local forecast, sun & moon, charts), **Sensors** (sensors, GPS),
  **Settings** (configuration, calibration, pins, RTC clock, services, network, SD card, update,
  public page) and **System** (logs, diagnostics, information); groups expand/collapse and on
  narrow screens the menu hides behind the **☰** button.
- **Dashboard** — live overview of all sensors, split into **zones: indoor / outdoor**
  (+ a wind-direction card as a N/NE/E… compass).
- **Calibration** — offset, custom name and zone for every channel.
- **External sensors** — OneWire bus scan, up to 8× DS18B20.
- **Configuration** — station, time/timezone (zone list + custom POSIX TZ string), NTP server
  and the Home Assistant / MQTT configurator (broker address, account, token) with a connection test.
- **Weather services** — simultaneous upload to **Weather Underground, PWSWeather, Windy,
  OpenWeather, ThingSpeak and a custom URL** (keys and interval set on the web page, a
  *Send now* button, an **outdoor-sensors-only** option and a main temperature channel selector).
- **Air quality** — indoor CO₂ / eCO₂ / TVOC (SCD4x + SGP30, auto-detection).
- **Pins** — GPIO assignment editor in the web UI: a single pin list laid out like the PCB
  (left column J1, right column J3), a function picker with a description per pin,
  conflict/reserved-pin checking and *Save & restart / Refresh / Restore defaults* buttons
  (stored in NVS).
- **Sensor legend** — below the pin list: the sensors whose drivers are currently compiled into
  the firmware, with bus, wiring hint, parameters and calibration method. Catalog entries without
  a driver are hidden by default (revealed by the *Show sensors without a driver* toggle).
  Bus tiles filter the list and **highlight the pins** used by a given bus; clicking a pin shows
  which sensors can be attached to it. Legend source: [docs/CZUJNIKI.md](docs/CZUJNIKI.md) →
  generator [tools/gen_legend.py](tools/gen_legend.py) → `data/legend.js`.
- **SD card** — file preview, download, delete and format from the web UI, plus a
  *Re-detect card* button (`POST /api/sd/remount` — remounts SPI/SDMMC without a reboot;
  interface and pin numbers are visible in the tab and in `/api/status`).
- **CSV logs** — `timestamp,datetime,temp,hum,press,light,ds0..ds7,rain,wind,vane,pm1,pm25,pm10,co2,eco2,tvoc`
  plus mirrored `xN_*` columns from RS485 nodes (full legend: [docs/CZUJNIKI.md](docs/CZUJNIKI.md)).
  Files live in `/logs/YYYY-MM.csv`; the `/logs` directory is created automatically (even on a
  fresh card). Sensors from the new modular drivers (those without their own column in the wide
  file) are stored in parallel in the **long journal** `/logs/extra-YYYY-MM.csv` as
  `timestamp,datetime,id,value` — so every new sensor works on charts and in logs without
  changing the CSV header.
- **Charts** — daily / weekly / monthly / yearly (mean, min, max).
- **RGB LED ring (RGBIC)** — 36 WS2812B LEDs show the weather condition by colour (colours are
  editable on the page); brightness is set manually and remembered (**LED RGB** tab).
- **PL / EN / DE languages** — a language switch in the header; the choice is remembered in the
  browser and the whole page (tabs, messages, charts) translates live.
- **Settings in NVS** — Wi-Fi, pins, calibration, MQTT and ring settings live in the NVS
  partition, so they **survive firmware updates and `uploadfs`**.
- **Public page** — `http://<ip>/public.html` with company advertising (temperature, humidity,
  PM2.5, etc.).
- **Network** — Wi-Fi settings (client + access point), static IP, network scan.
- **Update (OTA)** — firmware and filesystem upload from the browser (with a progress bar) or via
  `espota` from a computer.
- **Logs** — live view of the station work log (start, Wi-Fi, MQTT, SD, errors), download and clear.
- **Diagnostics** — cards with memory, network, MQTT, OTA and sensors (chip temperature, reset reason).
- **Backup** — export/import of the whole configuration to/from a JSON file.
- **Home Assistant** — automatic sensor discovery via MQTT discovery.
- **RS485 nodes** — the **ESP & RS485 bus** tab: the bus (master/node role, addresses, speed),
  a node list and each node with its external sensors (pins, channel calibration, configuration,
  log, diagnostics, actions such as reboot or time sync). Node data reaches the dashboard, charts,
  SD-card CSV files and Home Assistant.
- **PSRAM** — large JSON documents, file lists and the log buffer go to the 8 MB PSRAM (smooth UI).

### Supported sensors and drivers

| Group | Models (✅ = driver already in code, 🟡 = ready to add) |
|---|---|
| Temperature | ✅ BME280 · ✅ SHT40/41/45 (SHT4x) · ✅ BMP581 · ✅ DS18B20 ×8 · ✅ thermocouples K/J/T (MAX6675/MAX31855) · ✅ PT100/PT1000 (MAX31865) · ✅ MLX90614/90615 (via `drv_motion`) · 🟡 BME680/688, SHT3x/85, AHT1x/2x, HDC1080/2010, TMP117, MCP9808, NTC, MLX90632/90640/90641, Si7051 |
| Humidity | ✅ BME280 · ✅ SHT40/41/45 · ✅ DHT11/22, AM2301/2320 (GPIO 42) · 🟡 SHT3x/85, AHT2x, HDC2xxx |
| Pressure | ✅ BME280 · ✅ BMP581 · 🟡 BMP180/280/380/388/390, DPS310/368, LPS22/28, MPL3115A2, MS5611 |
| Rain | ✅ tipping-bucket (reed switch) + Hall/reed · 🟡 FC-37/YL-83, optical, weighing (HX711) |
| Wind (speed) | ✅ pulse/reed anemometer · 🟡 ultrasonic, differential pressure (Pitot) |
| Wind (direction) | ✅ ADC wind vane (16 directions, compass) · ✅ AS5600 (12-bit I²C encoder) · 🟡 AS5048A/B, AS5047P, MT6701, MA730/732, TLE5012B |
| Magnetometers | ✅ MMC5983MA (azimuth) · ✅ QMC5883L · ✅ HMC5883L · 🟡 HMC5983, LIS3MDL, LIS2MDL, MMC5603, BMM150/350, AK09918 |
| Hall sensors | ✅ reed switches · 🟡 A3144, AH3144, SS49E, A132x, DRV5032/5033/5055/5056, TMAG3001/5170 |
| Light (lux) | ✅ BH1750 · ✅ VEML7700 · 🟡 OPT3001/3002, LTR-329/303, TSL2591/2561, ISL29125, TCS34725 |
| UV | ✅ LTR-390UV (UVS channel) · 🟡 VEML6070, VEML6075, SI1145, GUVA-S12SD, GY-ML8511 |
| Pyranometer | ✅ calibrated cell via ADC or ADS1115 (W/m²) · 🟡 cell + shunt, MLX90614 pointed at the sky |
| Soil | ✅ DS18B20 · ✅ analogue YL-69 / capacitive probe (dry/wet calibration) · 🟡 SHT in soil, tensiometers, **SDI-12**, Modbus/RS485 (NPK, pH, EC) |
| Analog inputs | ✅ ADS1115 ×4 (16-bit, I²C 0x48–0x4B; channels `ads_0…ads_3` off by default) · ✅ MCP3008 (10-bit) / MCP3208 (12-bit) ×8 on bit-bang SPI (own `adc_cs` line, shared `tc_sck/tc_mosi/tc_miso` with thermocouples) |
| Particulate matter | ✅ PMS5003/7003/6003 · ✅ SPS30 · ✅ SEN50/54/55 (SEN5x) · 🟡 PMSA003I, SDS011/018, HPMA115, OPC-N3 |
| VOC / IAQ | ✅ SGP30 (eCO₂ + TVOC) · 🟡 BME680/688, SGP40/41, ENS160/161, CCS811 |
| CO₂ | ✅ **SCD30/40/41** (NDIR) · 🟡 SenseAir S8, Sunrise, MH-Z19B/C/D |
| Gases | ✅ MQ-2/3/4/5/6/7/8/9/131/135/136/137 (R0, RL, ppm chart) · ✅ MiCS-4514 (CO) / MiCS-6814 (NO₂) · ✅ TGS2600 · 🟡 TGS2602/2611, electrochemical cells |
| IR temperature | ✅ MLX90614/90615 · 🟡 MLX90632/90640/90641, AMG8833 |
| Water / snow level | ✅ VL53L0X / VL53L1X · 🟡 **L4CD**/**L5CX**/L7CX, HC-SR04, JSN-SR04T, A02YYUW, 4–20 mA hydrostatic probes |
| IMU | ✅ MPU6050/6500/9250/9255, LIS3DH, ADXL345 · 🟡 MPU6886, LIS2DW12, ADXL355, ICM-42688, BMI270, BNO055/085/086 |
| GPS | ✅ NEO-6M/7M/8M/M8N/M8Q/M9N/M9V, ATGM336H, L76K, L86, GT-U7 (NMEA 0183 over UART, time source) · 🟡 ZED-F9P (RTK/UBX), modules without NMEA |
| Sun | 🟡 Sun position (altitude, azimuth), sunrise/sunset, clear-sky factor |
| Power | ✅ ACS712 / ACS758 (current and power) · ✅ battery voltage measurement · 🟡 INA219/226/228/238/3221, MAX17048/49/55, LC709203F |
| Garden | ✅ leaf wetness **or** icing (one input, two channels) · 🟡 PAR (S2-131), daily rain counter |
| Radar / presence | 🟡 HLK-LD2410, **LD2450**, LD2461 |
| Sound / audio | ✅ I²S microphone INMP441 / ICS-43434 / SPH0645 (`snd_level`, `snd_peak`, `snd_leq`) · ✅ MAX98357A (TX installation) · 🟡 PDM/analogue |
| Station modules | ✅ WS2812B 36 px, microSD 8 GB · 🟡 OLED SSD1306/SH1106, LCD1602/2004, TFT/e-ink, buzzer/relay |

> Extra drivers (I²C/analog): **SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA, AS5600,
> ADS1115** (`src/drv_i2c.*`) and **SEN5x / SPS30** (`src/drv_pm.*`); the layer that connects
> them to channels, calibration and CSV is [src/sensors_extra.cpp](src/sensors_extra.cpp).
> Each one auto-detects at boot — no hardware = no channel, no error.

### Pin map (defaults)

| Module | Pins |
|---|---|
| I²C (BME280/BME680, SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA, AS5600, ADS1115, SCD4x, SGP30, SEN5x/SPS30, BH1750, AS3935) | SDA=8, SCL=9 |
| DS18B20 (OneWire) | 4 |
| Rain gauge | 5 (interrupt) |
| Anemometer | 6 (interrupt) |
| Wind vane (ADC) — disable for AS5600 | 1 |
| Soil moisture probe (ADC1) | 2 |
| Thermocouples / RTD / MCP3008-3208 (bit-bang SPI) | `tc_sck`, `tc_mosi`, `tc_miso` shared + separate CS lines: `tc_cs` (MAX6675/MAX31855), `rtd_cs` (MAX31865), `adc_cs` (MCP3008/MCP3208) — default −1, set in the pin editor |
| Gases (MQ / MiCS / TGS), leaf wetness, ACS712/758 | `gas_adc`, `gas_adc2`, `leaf_adc`, `acs_adc` — default −1; each can also read from ADS1115 (`*_ads_ch`) |
| I²S microphone / MAX98357A amplifier | `i2s_bclk`, `i2s_ws`, `i2s_din` / `i2s_dout` — default −1 |
| Pyranometer (ADC1) | −1 = disabled |
| PMS5003 (UART2) | RX=18, TX=17 |
| GPS (UART1, NMEA) | RX=15, TX=16 |
| AS3935 lightning detector | 14 (interrupt) |
| DHT11 / DHT22 / AM2302 (1-Wire) | 42 (+ 4.7 kΩ pull-up to 3V3) |
| SD (SPI, default) | CS=10, SCK=12, MOSI=11, MISO=13 |
| SDMMC 1-bit (option, off by default) | CLK=40, CMD=41, D0=42 — conflicts with DHT22 |
| LED / button | 21 / 0 (BOOT) |
| RGB LED ring | 7 (WS2812B data; 5 V and GND from the board) |

All of these can be changed **without recompiling** — the **Pins** tab on the web page (stored in
NVS + a `config.json` copy, applied after restart). [include/pins.h](include/pins.h) holds only
the default values.

> ⚠️ **N16R8 (QIO flash + octal PSRAM):** GPIO 26–37 are occupied by flash/PSRAM. Do not use
> them for sensors. The SD card runs over SPI (CS=10, SCK=12, MOSI=11, MISO=13); the optional
> SDMMC 1-bit mode uses GPIO 40–42 — if you enable it, move DHT22 away from GPIO 42.

### Build & flash

1. Install [PlatformIO](https://platformio.org/) (VS Code + PlatformIO IDE extension).
2. Open the project folder in VS Code.
3. Build and upload (board over USB):
   ```
   pio run -t upload
   ```
4. Upload the web filesystem:
   ```
   pio run -t uploadfs
   ```
5. Serial monitor:
   ```
   pio device monitor
   ```

#### Over-the-air update (no USB cable)

Two ways:

1. **From the browser** — **Update** tab: pick `firmware.bin` from `.pio/build/esp32s3/`
   (type: *Program*) or `littlefs.bin` (type: *Web page*), enter the OTA password and press *Upload*.
2. **From a computer (espota)** — first set the OTA password in the *Update* tab:
   ```
   pio run -e esp32s3-ota -t upload
   ```
   The default password in [platformio.ini](platformio.ini) is `stacja-ota` (`upload_flags = --auth`).

> Warning: with an empty OTA password anyone on the same network can flash the firmware.

- The image size is taken from the browser request, so only the needed range is erased — a firmware
  update takes several seconds, the 9.94 MB web filesystem about a minute.
- The filesystem image is automatically trimmed to the partition size.
- The upload is guarded: if the browser tab is closed or Wi-Fi drops, the station aborts the upload
  after 20 s of silence and keeps working (progress: `GET /api/ota/info`, diagnostics:
  `GET /api/ota/diag`).
- An update **does not erase NVS** — settings, pins and calibration remain.
- `espota` works by IP address (`pio run -e esp32s3-ota -t upload`); do not keep a serial monitor
  open on COM9 at the same time, because opening the port reboots the board and aborts the upload.

#### Board variants (master and node)

The main station (master) is built only for **N16R8** — it needs PSRAM for the log buffer and
9.94 MB LittleFS for the web page. **A node can be a different, cheaper board** — just pick a
ready PlatformIO environment and flash a file from `dist\`. The release contains **three
binaries**: master, S3 8 MB node (N8R4/N8R2) and C3 SuperMini node — that covers a whole install.

| Environment | Board | Flash / PSRAM | Partitions | Notes |
|---|---|---|---|---|
| `esp32s3-master` | ESP32-S3 WROOM-1 **N16R8** | 16 MB / 8 MB octal | `partitions.csv` | main station: www, MQTT, OTA, SD card |
| `esp32s3-node-8mb-quad` | ESP32-S3 **N8R4 / N8R2** | 8 MB / 4 or 2 MB quad | `partitions-8mb.csv` | **recommended node** — quad PSRAM (`qio_qspi`), full pin list |
| `esp32c3-node` | **ESP32-C3 SuperMini** | 4 MB / no PSRAM | `partitions-4mb.csv` | node only: no SD card and no PMS5003 (options hidden/disabled); factory address 2 |
| `esp32c3-node-addr3` … `-addr8` | as above | | | same binary with `-DSTACJA_NODE_ADDR=N` — factory address 3…8 for extra modules |
| `esp32s3-node` | ESP32-S3 **N16R8** | 16 MB / 8 MB octal | `partitions.csv` | node on a second identical board — **no ready binary**, build manually |
| `esp32s3-node-8mb` | ESP32-S3 **N8R8** | 8 MB / 8 MB octal | `partitions-8mb.csv` | 8 MB module with octal PSRAM — **no ready binary**, build manually |

**Does the node have to match the master?** No. The `n8r4` version is enough because a node does
not serve the web page or MQTT — firmware is the biggest memory consumer there. Pins and drivers
are **the same** (identical function table), so you can attach all sensors to a node. Limitations
of the smaller boards: the C3 has fewer GPIOs (max 21), only one ADC free of Wi-Fi and **does not
support microSD** or the PMS5003 particle sensor (needs a third UART).

Ready-made files (firmware + web) are assembled by:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_release.ps1
powershell -ExecutionPolicy Bypass -File tools\build_release.ps1 -PublishNode
```

Output goes to `dist\` (`…-master-16mb.bin`, `…-node-8mb-quad.bin`, `…-node-c3-4mb.bin`,
**`…-node-c3-4mb-addr3.bin` … `-addr8.bin`** (C3 nodes with a factory address), `-ota.bin`
variants and two LittleFS images: `…-www-16mb.bin` (page + node binaries) and `…-www-8mb.bin`
(page only). Files without `-ota` are flashed **from address 0x0** and **erase NVS** (a new
board); use `-ota.bin` to update a running station.

### First run

After flashing, the station starts an access point **StacjaPogody** — connect to it and open
`http://192.168.4.1` to configure Wi-Fi. The **Network** tab sets the router SSID/password, the
mDNS name (`<hostname>.local`) and, optionally, a **static IP** (IP, gateway, mask, DNS).
After saving, the station reboots; if the new address is unreachable, go back to
`http://192.168.4.1`. The station (ESP32-S3) sees **2.4 GHz networks only**.

### Sensor zones (indoor / outdoor)

Every measurement channel has a **zone**: `indoor` or `outdoor`. You change it in the
**Calibration** tab (the **Zone** column) and the change applies **immediately, without a reboot**.
The dashboard groups cards under *Indoor* and *Outdoor* headings, and weather services can upload
**outdoor sensors only**.

### Home Assistant

In `configuration.yaml` only the MQTT broker is required:

```yaml
mqtt:
  broker: <broker IP>
```

The station publishes discovery to `homeassistant/sensor/<prefix>_<id>/config`, so sensors appear
automatically. State on `<prefix>/sensor/<id>/state`, availability on `<prefix>/status`.

- In Home Assistant use the **Mosquitto broker** add-on; as the user, create a dedicated account for
  the station and paste a **long-lived access token** as the password (Profile → Long-lived tokens).
- The **Configuration** tab has a connection test with the result, e.g.
  `Connected to 192.168.1.10:1883 (41 ms)`.

**Weather services** (**Services** tab): Weather Underground, PWSWeather, Windy, OpenWeather,
ThingSpeak and a custom GET URL — interval (min 30 s, default 300 s), *Transfer active* switch,
*Send now* button, `all sensors` or `outdoor sensors only`, and a main temperature channel selector.

### Logs and diagnostics

- `GET /api/syslog` — full text log (PSRAM buffer, ~48 kB, oldest lines dropped).
- `DELETE /api/syslog` — clears the buffer.
- `GET /api/diagnostics` — JSON cards: System, Memory, Storage, Network, MQTT/HA, OTA, Sensors.
- The **Logs** tab refreshes every 2 s, shows up to 600 lines and colour-codes them.

### Backup and NVS settings

- `GET /api/backup` — downloads the whole configuration as `stacja-pogody-ustawienia.json`.
- `POST /api/backup/restore` — restores the configuration from JSON (Configuration tab).
- Settings (Wi-Fi/AP, static IP, pins, channel calibration, MQTT/HA, timezone, LED ring) are stored
  in the **NVS partition** by [src/nvs_store.cpp](src/nvs_store.cpp), so firmware updates and
  `uploadfs` **do not erase them**; a factory reset (Configuration tab) clears NVS together with
  the files.

### Calibration (after soldering)

Offsets and constants: BME280 temperature offset, `RAIN_MM_PER_TIP`, `ANEM_KMH_PER_HZ`, wind-vane
ADC thresholds, soil dry/wet voltages (`soil_dry_v`, `soil_wet_v`), `solar_mv_wm2`, magnetic
declination `mag_decl` — set in the **Calibration** tab / [src/sensors.cpp](src/sensors.cpp).
Full per-sensor details (I²C addresses, parameters, calibration, CSV columns):
[docs/CZUJNIKI.md](docs/CZUJNIKI.md).

### Languages (PL / EN / DE)

The whole web page works in three languages — the **Language / Sprache** switch in the header; the
choice is stored in `localStorage`. Mechanism: [data/i18n.js](data/i18n.js) with `SLOWNIK`
(dictionary), `FRAZY` (longest phrases) and `REGEX` (dynamically built strings). Coverage check:
`node tools/analyze_i18n.js`.

### RS485 nodes (external sensors)

The station can poll **one or several modules** (e.g. an ESP32-C3 SuperMini) over the data line —
currently a direct 3-wire TTL line (TX↔RX + common GND, no MAX485 converter). A node is built from
**the same code** as the master (role selected in configuration) and is **headless**
(`-DSTACJA_HEADLESS=1`): it reads sensors, serves the RS485 bus and logs to SD without Wi-Fi, AP,
MQTT, weather services, web or OTA. All node channels are mirrored on the master as `xN_<id>`
(N = node address), so they reach the dashboard, LED ring, Home Assistant, charts and CSV.

Master wiring (defaults): TX GPIO 38 → node RX 20, RX GPIO 39 → node TX 21, common GND (the full
table and troubleshooting are in the Polish section below).

### Screenshots

Full-page screenshots of the **English** interface are in the **Zrzuty ekranu / Screenshots**
section below (all 24 tabs: dashboard, forecast, sun & moon, charts, records, reports, sensors,
GPS, camera, configuration, calibration, alerts, pins, RTC, services, network, SD card, OTA,
public page, LED ring, RS485 bus, logs, diagnostics, information).

### License

© 2026 Kruzio. Published under the **PolyForm Noncommercial License 1.0.0** — non-commercial use,
copying and modification are allowed; you must keep the copyright notice and, for derivative works,
state your changes and link to this repository. Commercial use requires the author's written
consent. Full text: [LICENSE](LICENSE) (identical to [data/LICENSE.txt](data/LICENSE.txt), served
by the station as `/LICENSE.txt`). Third-party libraries keep their own licences (table in the
Polish section below).

---

<a id="polski"></a>

## 🇵🇱 Polski — pełny opis

## Funkcje

- **Menu boczne z podmenu** — nawigacja jest przewijana po lewej stronie i pogrupowana:
  **Prognoza** (prognoza lokalna, słońce i księżyc, wykresy), **Czujniki** (czujniki, GPS),
  **Ustawienia** (konfiguracja, kalibracja, piny, zegar RTC, serwisy, sieć, karta SD, aktualizacja,
  publiczny) i **System** (logi, diagnostyka, informacje); grupy rozwijają się i zwijają,
  a na wąskim ekranie menu chowa się za przyciskiem **☰**.
- **Pulpit** — podgląd wszystkich czujników na żywo, podzielony na **strefy: wewnątrz / na zewnątrz**
  (+ karta kierunku wiatru jako kompas N/NE/E…).
- **Kalibracja** — offset, własna nazwa i strefa dla każdego kanału.
- **Czujniki zewnętrzne** — skanowanie magistrali OneWire, do 8× DS18B20.
- **Konfiguracja** — stacja, czas/strefa czasowa (lista stref + własny zapis TZ), serwer NTP
  oraz konfigurator Home Assistant / MQTT (adres brokera, konto, token) z testem połączenia.
- **Serwisy pogodowe** — jednoczesna wysyłka do **Weather Underground, PWSWeather, Windy,
  OpenWeather, ThingSpeak i własnego adresu URL** (klucze i interwał na stronie www,
  przycisk „Wyślij teraz”, opcja **tylko czujniki zewnętrzne**, wybór kanału temperatury).
- **Jakość powietrza** — wewnętrzne czujniki CO₂ / eCO₂ / TVOC (SCD4x + SGP30, auto-wykrywanie).
- **Piny** — edytor przypisania GPIO z poziomu WWW: jedna lista pinów w układzie płytki
  (jak na laminacie, lewa kolumna J1 i prawa J3), przy każdym pinie wybór funkcji z opisem,
  kontrola konfliktów i pinów zarezerwowanych, przyciski *Zapisz i zrestartuj / Odśwież /
  Przywróć domyślne* pod listą (zapis w NVS).
- **Legenda czujników** — pod listą pinów: czujniki, których sterowniki są w tej chwili
  w firmware stacji, z magistralą, podpowiedzią podłączenia, parametrami i sposobem kalibracji.
  Pozycje z katalogu bez sterownika są domyślnie ukryte (odsłania je przełącznik
  *Pokaż też czujniki bez sterownika*). Kafelki magistral filtrują listę i **podświetlają piny**,
  przez które podłącza się daną magistralę; kliknięcie pinu pokazuje, jakie czujniki można
  na nim zawiesić. Źródło legendy: [docs/CZUJNIKI.md](docs/CZUJNIKI.md) → generator
  [tools/gen_legend.py](tools/gen_legend.py) → `data/legend.js`.
- **Karta SD** — podgląd plików, pobieranie, usuwanie, formatowanie z poziomu WWW,
  przycisk „Wykryj kartę ponownie” (`POST /api/sd/remount` — ponowny montaż SPI/SDMMC bez
  restartu stacji; interfejs i numery pinów widoczne w zakładce i w `/api/status`).
- **Logi CSV** — `timestamp,datetime,temp,hum,press,light,ds0..ds7,rain,wind,vane,pm1,pm25,pm10,co2,eco2,tvoc`
  plus kolumny lustrzane `xN_*` z węzłów RS485 (pełna legenda: [docs/CZUJNIKI.md](docs/CZUJNIKI.md)).
  Pliki leżą w `/logs/RRRR-MM.csv`, a katalog `/logs` stacja tworzy sama (również na świeżej karcie).
  Czujniki z nowych sterowników modułowych (te bez własnej kolumny szerokiego pliku) zapisują się
  równolegle w **dzienniku długim** `/logs/extra-RRRR-MM.csv` jako `timestamp,datetime,id,value` —
  każdy nowy czujnik działa więc na wykresach i w logach bez zmiany nagłówka CSV.
- **Wykresy** — dobowe / tygodniowe / miesięczne / roczne (średnia, min, max).
- **Pierścień LED RGB (RGBIC)** — 36 diod WS2812B pokazuje stan pogody kolorem
  (kolory stanów są edytowalne na stronie), jasność ustawiana ręcznie i zapamiętywana
  (zakładka **Led RGB**).
- **Wersje językowe PL / EN / DE** — przełącznik języka w nagłówku; wybór jest pamiętany
  w przeglądarce, a cała strona (zakładki, komunikaty, wykresy) tłumaczy się na żywo.
- **Ustawienia w NVS** — Wi-Fi, piny, kalibracja, MQTT i ustawienia pierścienia leżą
  w partycji NVS, więc **nie przepadają po aktualizacji firmware ani po wgraniu www**.
- **Strona publiczna** — `http://<ip>/public.html` z reklamą firmy (temp, wilgotność, PM2.5 itd.).
- **Sieć** — ustawienia Wi-Fi (klient + punkt dostępowy), stały adres IP, skanowanie sieci.
- **Aktualizacja (OTA)** — wgrywanie firmware i systemu plików z przeglądarki (z paskiem postępu)
  albo przez `espota` z komputera.
- **Logi** — podgląd na żywo dziennika pracy stacji (start, Wi-Fi, MQTT, SD, błędy), pobieranie i czyszczenie.
- **Diagnostyka** — karty z pamięcią, siecią, MQTT, OTA i czujnikami (temperatura układu, powód resetu).
- **Kopia zapasowa** — eksport/import całej konfiguracji do pliku JSON.
- **Home Assistant** — automatyczne wykrywanie czujników przez MQTT discovery.
- **Węzły po RS485** — zakładka **ESP i magistrala RS485**: magistrala (rola master/węzeł, adresy,
  prędkość), lista węzłów i każdy węzeł z czujnikami zewnętrznymi (piny, kalibracja kanałów,
  konfiguracja, log, diagnostyka, akcje typu restart lub synchronizacja czasu). Dane węzłów trafiają
  na pulpit, do wykresów, do CSV na karcie SD i do Home Assistant.
- **PSRAM** — duże dokumenty JSON, listy plików i bufor logów trafiają do 8 MB PSRAM (płynne UI).

## Sterowniki i obsługiwane czujniki

Pełna legenda (podłączenie, parametry, kalibracja, wykresy i logi dla **każdego** czujnika)
znajduje się w osobnym dokumencie: **[docs/CZUJNIKI.md](docs/CZUJNIKI.md)**.

| Grupa | Modele (✅ = sterownik już w kodzie, 🟡 = gotowy do dopisania) |
|---|---|
| Temperatura | ✅ BME280 · ✅ SHT40/41/45 (SHT4x) · ✅ BMP581 · ✅ DS18B20 ×8 · ✅ termopary K/J/T (MAX6675/MAX31855) · ✅ PT100/PT1000 (MAX31865) · ✅ MLX90614/90615 (przez `drv_motion`) · 🟡 BME680/688, SHT3x/85, AHT1x/2x, HDC1080/2010, TMP117, MCP9808, NTC, MLX90632/90640/90641, Si7051 |
| Wilgotność | ✅ BME280 · ✅ SHT40/41/45 · ✅ DHT11/22, AM2301/2320 (GPIO 42) · 🟡 SHT3x/85, AHT2x, HDC2xxx |
| Ciśnienie | ✅ BME280 · ✅ BMP581 · 🟡 BMP180/280/380/388/390, DPS310/368, LPS22/28, MPL3115A2, MS5611 |
| Deszcz | ✅ przechyłowy (bąbelkowy) + Hall/kontaktron · 🟡 FC-37/YL-83, optyczny, wagowy (HX711) |
| Wiatr (prędkość) | ✅ anemometr impulsowy/contactron · 🟡 ultradźwiękowy, różnica ciśnień (Pitot) |
| Wiatr (kierunek) | ✅ wiatrowskaz ADC (16 kierunków, kompas) · ✅ AS5600 (enkoder 12-bit na I²C) · 🟡 AS5048A/B, AS5047P, MT6701, MA730/732, TLE5012B |
| Magnetometry | ✅ MMC5983MA (azymut) · ✅ QMC5883L · ✅ HMC5883L · 🟡 HMC5983, LIS3MDL, LIS2MDL, MMC5603, BMM150/350, AK09918 |
| Czujniki Halla | ✅ kontaktron/reeds · 🟡 A3144, AH3144, SS49E, A132x, DRV5032/5033/5055/5056, TMAG3001/5170 |
| Światło (lux) | ✅ BH1750 · ✅ VEML7700 · 🟡 OPT3001/3002, LTR-329/303, TSL2591/2561, ISL29125, TCS34725 |
| UV | ✅ LTR-390UV (kanał UVS) · 🟡 VEML6070, VEML6075, SI1145, GUVA-S12SD, GY-ML8511 |
| Pyranometr | ✅ ogniwo kalibrowane przez ADC lub ADS1115 (W/m²) · 🟡 ogniwo + bocznik, MLX90614 na niebo |
| Gleba | ✅ DS18B20 · ✅ sonda analogowa YL-69 / pojemnościowa (kalibracja sucho/mokro) · 🟡 SHT w glebie, tensjometry, **SDI-12**, Modbus/RS485 (NPK, pH, EC) |
| Wejścia analogowe | ✅ ADS1115 ×4 (16 bit, I²C 0x48–0x4B; kanały `ads_0…ads_3` domyślnie wyłączone) · ✅ MCP3008 (10 bit) / MCP3208 (12 bit) ×8 na SPI bit-bang (własna linia `adc_cs`, wspólne `tc_sck/tc_mosi/tc_miso` z termoparami) |
| Pyły PM | ✅ PMS5003/7003/6003 · ✅ SPS30 · ✅ SEN50/54/55 (SEN5x) · 🟡 PMSA003I, SDS011/018, HPMA115, OPC-N3 |
| VOC / IAQ | ✅ SGP30 (eCO₂ + TVOC) · 🟡 BME680/688, SGP40/41, ENS160/161, CCS811 |
| CO₂ | ✅ **SCD30/40/41** (NDIR) · 🟡 SenseAir S8, Sunrise, MH-Z19B/C/D |
| Gazy | ✅ MQ-2/3/4/5/6/7/8/9/131/135/136/137 (R0, RL, wykres ppm) · ✅ MiCS-4514 (CO) / MiCS-6814 (NO₂) · ✅ TGS2600 · 🟡 TGS2602/2611, ogniwa elektrochemiczne |
| Temperatura IR | ✅ MLX90614/90615 · 🟡 MLX90632/90640/90641, AMG8833 |
| Poziom wody / śniegu | ✅ VL53L0X / VL53L1X · 🟡 **L4CD**/**L5CX**/L7CX, HC-SR04, JSN-SR04T, A02YYUW, sondy hydrostatyczne 4–20 mA |
| IMU | ✅ MPU6050/6500/9250/9255, LIS3DH, ADXL345 · 🟡 MPU6886, LIS2DW12, ADXL355, ICM-42688, BMI270, BNO055/085/086 |
| GPS | ✅ NEO-6M/7M/8M/M8N/M8Q/M9N/M9V, ATGM336H, L76K, L86, GT-U7 (NMEA 0183 po UART, źródło czasu) · 🟡 ZED-F9P (RTK/UBX), moduły bez NMEA |
| Słońce | 🟡 pozycja Słońca (wysokość, azymut), wschód/zachód, współczynnik „clear sky” |
| Zasilanie | ✅ ACS712 / ACS758 (prąd i moc) · ✅ pomiar napięcia baterii · 🟡 INA219/226/228/238/3221, MAX17048/49/55, LC709203F |
| Ogród | ✅ mokrość liścia **albo** oblodzenie (jedno wejście, dwa kanały) · 🟡 PAR (S2-131), licznik opadów dobowych |
| Radar / obecność | 🟡 HLK-LD2410, **LD2450**, LD2461 |
| Dźwięk / audio | ✅ mikrofon I²S INMP441 / ICS-43434 / SPH0645 (`snd_level`, `snd_peak`, `snd_leq`) · ✅ MAX98357A (instalacja TX) · 🟡 PDM/analogowe |
| Moduły stacji | ✅ WS2812B 36 px, SD microSD 8 GB · 🟡 OLED SSD1306/SH1106, LCD1602/2004, TFT/e-ink, buzzer/przekaźnik |

> Sterowniki dodatkowe (I²C/analog): **SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA,
> AS5600, ADS1115** (`src/drv_i2c.*`) oraz **SEN5x / SPS30** (`src/drv_pm.*`); warstwa
> spinająca je z kanałami, kalibracją i CSV to [src/sensors_extra.cpp](src/sensors_extra.cpp).
> Każdy z nich sam się wykrywa przy starcie — brak sprzętu = brak kanału, bez błędu.

> **Sterowniki modułowe** (kolejne zestawy czujników) mają wspólny kontrakt
> [src/drv_mod.h](src/drv_mod.h): plik `drv_*.cpp` opisuje swoje kanały (nazwa, jednostka,
> strefa, klasa HA) i wystawia jedną stałą `DrvModule`, a [src/drv_table.cpp](src/drv_table.cpp)
> wypisuje je w jednej tablicy. Kod stacji sam rejestruje kanały, woła `begin()` przy starcie
> i `read()` w każdym cyklu — dzięki temu nowy sterownik działa od razu na **obu** ESP
> (master i węzeł RS485 buduje się z tych samych źródeł).
> Gotowe moduły: **`drv_tc`** (termopary + PT100/RTD + MCP3008/MCP3208), **`drv_gas`**
> (MQ/MiCS/TGS, ACS712/758, liść/oblodzenie), **`drv_motion`** (IMU, magnetometry, ToF, IR),
> **`drv_i2s`** (mikrofon I²S + MAX98357A). Każdy z nich sam się wykrywa — brak sprzętu = brak kanału.
> ⚠️ Sterowniki nowej fali (termopary, PT100, MCP3x08, gazy, IMU/ToF, I²S) kompilują się i mają
> komplet kanałów, ale **nie były jeszcze sprawdzone na fizycznym sprzęcie** — lista rzeczy do
> potwierdzenia jest w [docs/TODO.md](docs/TODO.md).
> Pełny katalog czujników z podpowiedziami podłączenia: [docs/CZUJNIKI.md](docs/CZUJNIKI.md)
> (rozdziały 33–34 opisują I²S/audio oraz pozostałe magistrale, ekspandery i moduły wykonawcze).

> Każdy czujnik jest **opcjonalny** — wykrywany przy starcie, brak = brak kanału (bez błędu).
> Nowy sterownik dodaje się w 5 krokach opisanych w [docs/CZUJNIKI.md](docs/CZUJNIKI.md#wykresy-i-logi-dla-nowych-czujników--jak-dodać-kanał).

## Piny (edytuj w [include/pins.h](include/pins.h) po zlutowaniu)

| Moduł | Piny |
|---|---|
| I2C (BME280/BME680, SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA, AS5600, ADS1115, SCD4x, SGP30, SEN5x/SPS30, BH1750, AS3935) | SDA=8, SCL=9 |
| DS18B20 (OneWire) | 4 |
| Deszczomierz | 5 (przerwanie) |
| Anemometr | 6 (przerwanie) |
| Wiatrowskaz (ADC) — wyłącz dla AS5600 | 1 |
| Sonda wilgotności gleby (ADC1) | 2 |
| Termopary / RTD / MCP3008-3208 (SPI bit-bang) | `tc_sck`, `tc_mosi`, `tc_miso` wspólne + osobne linie CS: `tc_cs` (MAX6675/MAX31855), `rtd_cs` (MAX31865), `adc_cs` (MCP3008/MCP3208) — domyślnie −1, ustaw w edytorze pinów |
| Gazy (MQ / MiCS / TGS), mokrość liścia, ACS712/758 | `gas_adc`, `gas_adc2`, `leaf_adc`, `acs_adc` — domyślnie −1; każde z nich może też czytać z ADS1115 (`*_ads_ch`) |
| Mikrofon I²S / wzmacniacz MAX98357A | `i2s_bclk`, `i2s_ws`, `i2s_din` / `i2s_dout` — domyślnie −1 |
| Pyranometr (ADC1) | −1 = wyłączony |
| PMS5003 (UART2) | RX=18, TX=17 |
| GPS (UART1, NMEA) | RX=15, TX=16 |
| Detektor wyładowań AS3935 | 14 (przerwanie) |
| DHT11 / DHT22 / AM2302 (1-Wire) | 42 (+ rezystor 4,7 kΩ do 3V3) |
| SD (SPI, podstawowy) | CS=10, SCK=12, MOSI=11, MISO=13 |
| SDMMC 1-bit (opcja, domyślnie wyłączony) | CLK=40, CMD=41, D0=42 — koliduje z DHT22 |
| LED / przycisk | 21 / 0 (BOOT) |
| Pierścień LED RGB | 7 (dane WS2812B; 5 V i GND z płytki) |

Wszystkie te przypisania można zmienić **bez rekompilacji** — zakładka **Piny** na stronie www
(zapis w NVS + kopia `config.json`, działa po restarcie). Plik [include/pins.h](include/pins.h) zawiera tylko wartości domyślne.

> ⚠️ **N16R8 (flash QIO + PSRAM octal):** GPIO 26–37 są zajęte przez flash/PSRAM.
> Nie używaj ich dla czujników. Karta SD pracuje po SPI (CS=10, SCK=12, MOSI=11, MISO=13),
> a opcjonalny tryb SDMMC 1-bit używa GPIO 40–42 — jeśli go włączysz, przenieś DHT22 z GPIO 42.

## Budowanie i wgrywanie

1. Zainstaluj [PlatformIO](https://platformio.org/) (VS Code + rozszerzenie PlatformIO IDE).
2. Otwórz folder projektu w VS Code.
3. Zbuduj i wgraj (płytka przez USB):
   ```
   pio run -t upload
   ```
4. Wgraj system plików z interfejsem WWW:
   ```
   pio run -t uploadfs
   ```
5. Monitor portu szeregowego:
   ```
   pio device monitor
   ```

### Aktualizacja przez Wi-Fi (bez kabla USB)

Dwa sposoby:

1. **Z przeglądarki** — zakładka **Aktualizacja**: wybierz `firmware.bin` z `.pio/build/esp32s3/`
   (rodzaj: *Program*) lub `littlefs.bin` (rodzaj: *Strona www*), wpisz hasło OTA i kliknij *Wgraj*.
2. **Z komputera (espota)** — najpierw wpisz hasło OTA w zakładce *Aktualizacja*:
   ```
   pio run -e esp32s3-ota -t upload
   ```
   Domyślne hasło w [platformio.ini](platformio.ini) to `stacja-ota` (`upload_flags = --auth`).

> Ostrzeżenie: przy pustym haśle OTA każdy w tej samej sieci może wgrać firmware.
> W zakładce *Diagnostyka* kartę „Aktualizacje (OTA)” warto sprawdzić po pierwszym uruchomieniu.

Jak to działa (i co warto wiedzieć przed wgraniem):

- Rozmiar obrazu jest brany z żądania przeglądarki, więc kasowany jest tylko potrzebny zakres —
  aktualizacja firmware trwa kilkanaście sekund, a system plików www (9,94 MB) około minuty.
- Obraz systemu plików jest automatycznie przycinany do rozmiaru partycji; nie trzeba pilnować
  zgodności rozmiaru pliku z tabelą partycji.
- Zapis jest pilnowany: jeśli karta przeglądarki zostanie zamknięta albo padnie Wi-Fi, stacja
  sama przerywa wgrywanie po 20 s ciszy, przywraca system plików do użytku i dalej pracuje
  (podgląd postępu: `GET /api/ota/info`, diagnostyka: `GET /api/ota/diag`).
- Aktualizacja **nie kasuje NVS** — ustawienia, piny i kalibracje zostają.
- `espota` z komputera działa po adresie IP (`pio run -e esp32s3-ota -t upload`); nie trzymaj
  wtedy otwartego monitora portu szeregowego COM9, bo otwarcie portu restartuje płytkę
  i przerywa wgrywanie.

### Warianty płytek (master i węzeł)

Stacja główna (master) jest budowana wyłącznie dla **N16R8** — potrzebuje PSRAM na bufor logów
i 9,94 MB LittleFS na stronę WWW. **Węzeł może być inną, tańszą płytką** — wystarczy
wybrać gotowe środowisko PlatformIO i wgrać plik z `dist\`. Wydanie zawiera **trzy binarki**:
master, węzeł S3 8 MB (N8R4/N8R2) i węzeł C3 SuperMini — to pokrywa całą instalację.

| Środowisko | Płytka | Flash / PSRAM | Partycje | Uwagi |
|---|---|---|---|---|
| `esp32s3-master` | ESP32-S3 WROOM-1 **N16R8** | 16 MB / 8 MB octal | `partitions.csv` | stacja główna: www, MQTT, OTA, karta SD |
| `esp32s3-node-8mb-quad` | ESP32-S3 **N8R4 / N8R2** | 8 MB / 4 albo 2 MB quad | `partitions-8mb.csv` | **zalecany węzeł** — PSRAM quad (`qio_qspi`), pełna lista pinów |
| `esp32c3-node` | **ESP32-C3 SuperMini** | 4 MB / bez PSRAM | `partitions-4mb.csv` | tylko węzeł: bez karty SD i PMS5003 (opcje ukryte/wyłączone); adres fabryczny 2 |
| `esp32c3-node-addr3` … `-addr8` | jak wyżej | | | ta sama binarka z `-DSTACJA_NODE_ADDR=N` — adres fabryczny 3…8 dla kolejnych modułów |
| `esp32s3-node` | ESP32-S3 **N16R8** | 16 MB / 8 MB octal | `partitions.csv` | węzeł na drugiej takiej samej płytce — **bez gotowej binarki**, buduj ręcznie |
| `esp32s3-node-8mb` | ESP32-S3 **N8R8** | 8 MB / 8 MB octal | `partitions-8mb.csv` | moduł 8 MB z PSRAM octal — **bez gotowej binarki**, buduj ręcznie |

**Czy węzeł musi być taki sam jak master?** Nie. Wersja `n8r4` wystarcza, bo węzeł nie serwuje
strony WWW ani MQTT — największym odbiorcą pamięci jest tam firmware. Piny i sterowniki są
**te same** (identyczna tabela funkcji), więc na węźle podłączysz wszystkie czujniki.
Ograniczenia mniejszych płytek: C3 ma mniej GPIO (maks. 21), tylko jedno ADC
wolne od Wi-Fi i **nie obsługuje karty microSD** ani czujnika pyłu PMS5003 (potrzebny trzeci UART).
Zamiast binarki dla drugiego N16R8/N8R8 możesz też wgrać plik `master-16mb.bin` i na stronie
www w zakładce **ESP i magistrala RS485** przestawić rolę na węzeł.

Gotowe pliki do wgrania (firmware + strona) składa skrypt:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_release.ps1
powershell -ExecutionPolicy Bypass -File tools\build_release.ps1 -PublishNode
```

Wynik trafia do `dist\` (`…-master-16mb.bin`, `…-node-8mb-quad.bin`,
`…-node-c3-4mb.bin`, **`…-node-c3-4mb-addr3.bin` … `-addr8.bin`** (węzły C3
z adresem fabrycznym), wersje `-ota.bin` oraz dwa obrazy LittleFS: `…-www-16mb.bin`
(strona razem z binarkami węzłów) i `…-www-8mb.bin` (sama strona — na płytce 8 MB
katalog `data\node\` o rozmiarze ~4,6 MB nie mieści się na partycji 1,88 MB).
Pliki bez `-ota` wgrywa się **od adresu 0x0** i **kasują one NVS** (nowa płytka), do aktualizacji
działającej stacji służy `-ota.bin`. Opcja `-PublishNode` dodatkowo kopiuje obrazy węzłów do
`data\node\` (przed zbudowaniem obrazów LittleFS, więc trafiają także do `…-www-16mb.bin`),
dzięki czemu stacja wydaje je sama z zakładki **ESP i magistrala RS485 → Pliki do wgrania**. Pamiętaj,
że po `-PublishNode` trzeba wgrać stronę (`pio run -t uploadfs`).

## Pierwsze uruchomienie

Stacja startuje w trybie AP+STA:

- **Punkt dostępowy:** `StacjaPogody` / hasło `12345678` (IP `192.168.4.1`).
- Połącz się z AP i otwórz `http://192.168.4.1` — tam skonfigurujesz Wi-Fi i MQTT.
- Jeśli skonfigurowano Wi-Fi, stacja dołącza do sieci i działa pod adresem z DHCP.

### Adresacja i sieć

- Zakładka **Sieć** ustawia SSID/hasło routera, nazwę mDNS (`<hostname>.local`) oraz — opcjonalnie —
  **stały adres IP** (IP, brama, maska, DNS). Bez tego stacja korzysta z DHCP.
- Ustawienia AP (SSID, hasło, adres IP, ukrycie SSID, „trzymaj AP na stałe”) są w tej samej zakładce.
- Po zapisie stacja restartuje — jeśli nowy adres jest nieosiągalny, wróć na `http://192.168.4.1`.
- Skanowanie sieci Wi-Fi z poziomu WWW (przycisk *Skanuj sieci*) działa asynchronicznie
  i pokazuje siłę sygnału oraz kanał.
- Stacja (ESP32-S3) widzi **wyłącznie sieci 2,4 GHz** — sieci 5 GHz nie pojawią się na liście.
  Skaner korzysta z `WiFi.scanNetworks()` z rdzenia Arduino, ponieważ własne wywołanie
  `esp_wifi_scan_start()`/`esp_wifi_scan_get_ap_records()` zawsze zwracało pustą listę:
  obsługa zdarzenia `WIFI_EVENT_SCAN_DONE` w rdzeniu (`WiFiScanClass::_scanDone`) sama pobiera
  i czyści wyniki sterownika, więc w naszym kodzie nie było już czego odczytać.
- Jeden przebieg skanu trwa kilka sekund (stacja skanuje wszystkie 13 kanałów); strona odświeża
  wynik co 1,2 s i pokazuje liczbę przebiegów.

## Strefy czujników (wewnątrz / na zewnątrz)

Każdy kanał pomiarowy ma przypisaną **strefę**: `wewnątrz` (ang. *Indoor*) lub `na zewnątrz`
(*Outdoor*). Strefę zmieniasz w zakładce **Kalibracja** (kolumna **Strefa**), a zmiana
działa **natychmiast, bez restartu** stacji.

- Pulpit (**Dashboard**) grupuje karty pod nagłówkami *Wewnątrz* i *Na zewnątrz*
  (kanały skonfigurowane, ale jeszcze nie odczytane, trafiają pod *Czeka na dane*).
- Wysyłka na zewnętrzne serwisy pogodowe może korzystać z opcji **tylko czujniki zewnętrzne**
  — wtedy do Internetu idą wyłącznie kanały ze strefy *na zewnątrz*.
- Strefy są zapisywane w NVS (`channels.<id>.zone`) i przechodzą przez kopię zapasową
  ustawień oraz aktualizację firmware.

Domyślne strefy: temperatura, wilgotność, ciśnienie, CO₂/eCO₂/TVOC → *wewnątrz*;
światło, DS18B20 zewnętrzne, opad, wiatr, wiatrowskaz, PM1/PM2.5/PM10 → *na zewnątrz*.

## Home Assistant

W pliku `configuration.yaml` wystarczy broker MQTT:

```yaml
mqtt:
  broker: <IP brokera>
```

Stacja publikuje discovery na `homeassistant/sensor/<prefix>_<id>/config`, więc czujniki
pojawią się automatycznie. Stan na `<prefix>/sensor/<id>/state`, dostępność na `<prefix>/status`.

### Konfigurator na stronie www

Zakładka **Konfiguracja** zawiera osobne sekcje:

- **Czas i strefa czasowa** — lista typowych stref (POSIX TZ, np. `CET-1CEST,M3.5.0,M10.5.0/3`)
  albo własny zapis, serwer NTP oraz podgląd aktualnego czasu stacji i stanu synchronizacji.
- **Home Assistant / MQTT** — adres brokera (IP Home Assistant), port, użytkownik,
  hasło/długoterminowy token HA, prefiks tematów, podgląd wynikowych tematów MQTT
  i przyciski **Zapisz i przetestuj** / **Testuj połączenie**.

Test połączenia wyłącza i ponownie włącza klienta MQTT z nowymi ustawieniami i pokazuje wynik
np. `Połączono z 192.168.1.10:1883 (41 ms)` albo `Brak połączenia ... [kod 4]`.
Gdy test nie przejdzie, stacja pokaże komunikat z kodem błędu, a ustawienia pozostają zapisane
(możesz je poprawić i przetestować ponownie).

> W Home Assistant: dodatek **Mosquitto broker**. Jako użytkownika podaj osobne konto dla stacji,
> a jako hasło wklej **długoterminowy token** HA (Profil → Tokeny długoterminowe).
> Zmiana brokera/strefy obowiązuje po restarcie stacji (przycisk **Restart**).

### Serwisy pogodowe (wysyłka na zewnątrz)

Zakładka **Serwisy** pozwala wysyłać pomiary jednocześnie do kilku serwisów:

| Serwis | Czego potrzebuje | Endpoint |
|---|---|---|
| Weather Underground | ID stacji + klucz | `weatherstation.wunderground.com` |
| PWSWeather | ID stacji + klucz | `api.pwsweather.com` |
| Windy | klucz API | `stations.windy.com` |
| OpenWeather | klucz API + ID stacji | `api.openweathermap.org` |
| ThingSpeak | klucz zapisu (write key) | `api.thingspeak.com` |
| Własny adres URL | pełny adres (metoda GET) | Twój serwer / Home Assistant |

- **Interwał** wysyłki w sekundach (min. 30, domyślnie 300 — serwisy i tak przyjmują dane
  rzadziej), przełącznik **Transfer aktywny** i przycisk **Wyślij teraz**.
- **Zakres danych**: `wszystkie czujniki` albo `tylko czujniki zewnętrzne` (strefa *na zewnątrz*).
- **Kanał temperatury dla serwisów**: który czujnik ma iść jako temperatura główna — `automatycznie`
  (stacja sama wybiera najpierw kanał zewnętrzny) lub konkretny kanał.
- Na liście widać stan każdego serwisu: kod odpowiedzi HTTP, wiek ostatniej wysyłki
  i komunikat błędu (np. *zły użytkownik lub hasło/token*, *brak połączenia z Wi-Fi*).

REST API: `GET /api/services` (konfiguracja + stan), `POST /api/services` (zapis tylko
zmienionych pól — klucze nieobecne w żądaniu zostają), `POST /api/services/test`
(wysyłka natychmiastowa, używana przez przycisk *Wyślij teraz*).

## Logi i diagnostyka

- `GET /api/syslog` — pełny dziennik w postaci tekstu (Pamięć PSRAM, ~48 kB, najstarsze linie wypadają).
- `DELETE /api/syslog` — czyści bufor.
- `GET /api/diagnostics` — kart JSON: System, Pamięć, Pamięć masowa, Sieć, MQTT/HA, OTA, Czujniki.
- Zakładka **Logi** odświeża się co 2 s, pokazuje do 600 ostatnich linii i koloruje je
  (niebieski = INFO, żółty = WARN, czerwony = ERROR, zielony = start, mocny czerwony = crash).

> Dlaczego własny logger, a nie `ESP_LOG`? Prekompilowane SDK dla wariantu `qio_opi` ma
> `CONFIG_LOG_MAXIMUM_LEVEL 1`, więc komunikaty INFO/WARN z bibliotek są usuwane na etapie
> kompilacji. Dlatego firmware używa własnego modułu [src/syslog.cpp](src/syslog.cpp),
> który dodatkowo przechwytuje logi IDF przez `esp_log_set_vprintf()`.

## Kopia zapasowa ustawień

- `GET /api/backup` — pobiera całą konfigurację jako `stacja-pogody-ustawienia.json`.
- `POST /api/backup/restore` — wgrywa konfigurację z pliku JSON (zakładka **Konfiguracja**).

## Pamięć ustawień (NVS) — przetrwa aktualizację

Ustawienia (Wi-Fi i AP, stały adres IP, pins, kalibracja kanałów, MQTT/HA, strefa czasowa,
jasność i kolory pierścienia LED) są zapisywane **w partycji NVS** przez
[src/nvs_store.cpp](src/nvs_store.cpp), a nie w pliku na LittleFS:

- aktualizacja firmware (OTA lub kablem) oraz wgranie nowych plików www **nie kasują NVS**,
  więc po aktualizacji stacja wstaje z tym samym Wi-Fi, adresem IP i kalibracją;
- duże dokumenty JSON są dzielone na kawałki po 3500 B (limit wpisu NVS ≈ 4000 B),
  a wpis jest opatrzony licznikiem i znacznikiem poprawności — przerwanie zapisu
  (reset, zanik zasilania) nie zostawia nadpisanej połowy konfiguracji;
- `config.json` na LittleFS pozostaje **kopią zapasową** i źródłem danych przy pierwszej
  aktualizacji ze starszej wersji (automatyczna migracja); licznik deszczu (`state.json`)
  też jest przeniesiony do NVS, żeby sumy opadów nie znikały po `uploadfs`;
- w logu startowym widać `Ustawienia z NVS (696 B)`, a w zakładce **Diagnostyka** —
  rozmiar partycji i liczbę wolnych wpisów NVS;
- **Przywrócenie ustawień fabrycznych** (zakładka *Konfiguracja*) czyści NVS razem z plikami.

## Pierścień LED RGB (status pogody)

WS2812B / RGBIC, 36 diod, 48 mm — **GPIO 7** (dane), zasilanie 5 V i GND z płytki.

| Kolor | Stan | Warunek (progi w [src/led_ring.cpp](src/led_ring.cpp)) |
|---|---|---|
| 🟢 zielony | Ładna pogoda | brak opadu, wiatr < 25 km/h, wilgotność < 80 % |
| 🟡 żółty | Pogoda średnia | pochmurno (jasność < 2000 lx) lub wilgotność ≥ 80 % |
| 🟠 bursztynowy | Wietrznie | wiatr ≥ 25 km/h |
| 🔵 jasnoniebieski | Deszcz | opad > 0,2 mm lub opad > 0,5 mm i wiatr ≥ 30 km/h |
| 🔵 ciemnoniebieski | Burza | opad ≥ 8 mm lub wiatr ≥ 45 km/h |
| ⚪ biały | Śnieg | opad > 0,2 mm przy temperaturze ≤ 0,5 °C |
| ⚫ szary | Mgła | wilgotność ≥ 95 % |
| 🟠 pomarańczowy | Upał | temperatura ≥ 30 °C |
| 🟣 fioletowy | Brak danych | żaden czujnik nie odpowiada |
| ⚫ wyłączony | LED wyłączony | `rgb_enabled = false` |

- Jasność (0–100 %) ustawia się suwakiem na stronie i **zapisuje w NVS** — po restarcie
  i po aktualizacji firmware wraca ta sama wartość. Efekty (pulsowanie, tęcza przy burzy)
  i tryb „przygaszenie w nocy” (22:00–06:00) można wyłączyć.
- Zakładka **Led RGB** pokazuje podgląd pierścienia na żywo, legendę kolorów
  (klik = test danego stanu) oraz zużycie prądu: *teraz* i *maksymalne*.
- **Prąd:** biała dioda pobiera ~60 mA, więc 36 diod to nawet 2,16 A. Firmware sam ogranicza
  prąd do `rgb_max_ma` (domyślnie 900 mA), ale przy pełnej jasności **zasil pierścień
  z osobnego zasilacza 5 V** (wspólna masa z płytką). W linii danych rezystor 330 Ω,
  między 5 V a GND kondensator 1000 µF.

## Kalibracja czujników (po zlutowaniu)

1. **BME280** — wstępnie skalibrowany fabrycznie; ewentualnie offset temperatury w zakładce *Kalibracja*.
2. **Deszczomierz** — stała `RAIN_MM_PER_TIP` w [src/sensors.cpp](src/sensors.cpp) (0.3 mm/impuls — sprawdź w specyfikacji swojego czujnika).
3. **Anemometr** — stała `ANEM_KMH_PER_HZ` (2.4 km/h na impuls/s — typowa wartość).
4. **Wiatrowskaz** — tabela ADC→kąt w `readVane()` w [src/sensors.cpp](src/sensors.cpp) jest szablonem; zmierz napięcia na swoim dzielniku rezystorowym i uzupełnij progi.
5. **BH1750** — adres `0x23` (lub `0x5C`), tryb ciągły 1 lx.
6. **Gleba** — dwa napięcia: `soil_dry_v` (sonda w suchym powietrzu/ziemi) i `soil_wet_v`
   (sonda w wodzie); wilgotność liczona liniowo między nimi.
7. **Pyranometr / ogniwo** — czułość `solar_mv_wm2` (mV na W/m²) z noty ogniwa albo
   z porównania ze wzorcem; przy wartości 0 kanał `solar_wm2` nie jest publikowany.
8. **Kompas MMC5983MA** — deklinacja magnetyczna `mag_decl` (np. +4,5° dla Polski),
   dodawana do azymutu magnetycznego.
9. **SHT4x / BMP581 / VEML7700 / LTR-390UV / SEN5x / SPS30 / ADS1115** — skalibrowane
   fabrycznie; korekty ustawiasz offsetem kanału, a wejścia `ads_0…ads_3` (domyślnie
   wyłączone) włączasz w *Kalibracja* dopiero, gdy je wykorzystasz.

Wartości z punktów 6–8 zapisuje sekcja **Kalibracja dodatkowych czujników** (NVS:
`soil_dry_v`, `soil_wet_v`, `mag_decl`, `solar_mv_wm2`; `POST /api/calibrate` → `{"extra":{…}}`).

Szczegółowy opis każdego czujnika (adresy I²C, parametry, sposób kalibracji i kolumny
w CSV) znajduje się w [docs/CZUJNIKI.md](docs/CZUJNIKI.md).

## Wersje językowe (PL / EN / DE)

Cała strona www działa w trzech językach — przełącznik **Język / Language / Sprache** w nagłówku,
wybór zapisany w `localStorage`. Tłumaczone są również napisy tworzone dynamicznie (karty
pulpitu, tabele, komunikaty błędów, legenda kolorów), a także komunikaty z firmware
pokazywane w interfejsie (błędy Wi-Fi, MQTT, SD, OTA).

Mechanizm: [data/i18n.js](data/i18n.js)

- `SLOWNIK` — słownik `"napisz źródłowy (PL)": ["English", "Deutsch"]`, kluczem jest
  **dokładny polski napis** z HTML/JS (dzięki temu nie trzeba niczego opisywać identyfikatorami).
- `FRAZY` — lista najdłuższych zwrotów do tłumaczenia fragmentów zdań (np. w zdaniach z liczbami).
- `REGEX` — reguły dla napisów budowanych dynamicznie (`"12 s temu"`, `"Zewn. DS18B20 #2"`,
  `"PM2.5 [µg/m³]"`). **Kolejność ma znaczenie** — reguły szczegółowe muszą być przed ogólnymi.
- Tłumaczenie działa przez spacer po DOM-ie + `MutationObserver`, więc nowe elementy
  są tłumaczone od razu po dodaniu. **Ważne:** jeśli składasz napis z kilku części, każda
  część musi być osobnym elementem/tekstem — sklejony łańcuch nie zostanie przetłumaczony.

Dodawanie nowego napisu: wstaw tekst po polsku w `data/index.html` i dopisz klucz do `SLOWNIK`.
Pokrycie słownika sprawdzisz skryptem:

```bash
node tools/analyze_i18n.js
```

Raport (duplikaty kluczy + brakujące tłumaczenia w firmware i na www) zapisuje się
w `%TEMP%\i18n_check.txt` (na Linuksie `/tmp/i18n_check.txt`).

Język interfejsu **nie zmienia** nazw encji wysyłanych do Home Assistant (te biorą się z nazw
kanałów ustawionych w zakładce *Kalibracja*) ani treści logów zapisywanych przez firmware.

## Zrzuty ekranu / Screenshots (interfejs angielski)

Poniżej pełny podgląd strony www w wersji **angielskiej** (przełącznik
*Język / Language / Sprache* → **English**). Zrzuty obejmują całe menu boczne
oraz wszystkie zakładki konfiguracji, kalibracji, czujników, GPS, kamery,
wykresów, logów i karty SD.

### Dashboard / Pulpit

![Dashboard](docs/images/dashboard.png)

### Forecast / Prognoza lokalna

![Forecast](docs/images/forecast.png)

### Sun and Moon / Słońce i Księżyc

![Sun and Moon](docs/images/astro.png)

### Charts / Wykresy

![Charts](docs/images/charts.png)

### Records / Rekordy i statystyki

![Records](docs/images/records.png)

### Reports / Raporty min/śr./maks.

![Reports](docs/images/reports.png)

### Sensors / Czujniki

![Sensors](docs/images/sensors.png)

### GPS

![GPS](docs/images/gps.png)

### Camera / Kamera

![Camera](docs/images/camera.png)

### Configuration / Konfiguracja

![Configuration](docs/images/config.png)

### Calibration / Kalibracja

![Calibration](docs/images/calibration.png)

### Alerts / Alarmy

![Alerts](docs/images/alerts.png)

### Pins / Piny

![Pins](docs/images/pins.png)

### RTC clock / Zegar RTC

![RTC clock](docs/images/rtc.png)

### Weather services / Serwisy wysyłki

![Weather services](docs/images/services.png)

### Network / Sieć Wi-Fi

![Network](docs/images/network.png)

### SD card / Karta SD

![SD card](docs/images/sd.png)

### Update (OTA) / Aktualizacja

![Update](docs/images/ota.png)

### Public page / Strona publiczna

![Public page](docs/images/public.png)

### LED ring / Pierścień LED RGB

![LED ring](docs/images/ring.png)

### RS485 bus / Magistrala RS485

![RS485 bus](docs/images/esp485.png)

### Logs / Logi

![Logs](docs/images/logs.png)

### Diagnostics / Diagnostyka

![Diagnostics](docs/images/diag.png)

### Information / Informacje

![Information](docs/images/info.png)

> Zrzuty wykonano z żywego urządzenia (master, IP `192.168.1.143`). Wartości
> czujników na obrazkach są przykładowe i zależą od podłączonego sprzętu.

## Węzły po RS485 (czujniki zewnętrzne)

Stacja może odpytywać **jeden albo kilka modułów** (np. ESP32-C3 SuperMini) po linii danych.
W obecnej wersji sprzętowej jest to bezpośrednia linia TTL 3-przewodowa (TX↔RX + wspólny GND,
bez konwertera MAX485); protokół jest ten sam, co dla magistrali RS485, więc para konwerterów
MAX485/SP3485 wystarczy później, gdy dojdzie prawdziwe urządzenie różnicowe (Modbus RTU).
Każdy moduł pełni rolę **węzła**: ma własne czujniki (np. zewnętrzne DS18B20), a jego
pomiary trafiają do mastera, który pokazuje je na pulpicie (osobna sekcja dla każdego adresu, np.
*ESP #2 (Adres 2)*), w wykresach, w plikach CSV na karcie SD i wysyła do Home Assistant. Węzeł nie
potrzebuje Wi-Fi — dane idą wyłącznie przewodem, a wszystkie jego ustawienia zmienia się
z zakładki **ESP i magistrala RS485** na masterze.

Węzeł buduje się z **tego samego kodu** co master (rola wybierana w konfiguracji), więc ma
**identyczną tabelę funkcji pinów, identyczne wartości domyślne i wszystkie sterowniki
czujników** — na węźle można podłączyć dokładnie ten sam zestaw czujników co na masterze.
Wydawane binarki węzła są **headless** (`-DSTACJA_HEADLESS=1`): węzeł tylko czyta czujniki,
obsługuje magistralę RS485 i loguje na kartę SD, a **nie** uruchamia Wi-Fi, punktu dostępowego,
MQTT, serwisów pogodowych, strony WWW ani OTA — dzięki temu na C3 SuperMini zostaje ~174 kB
wolnego RAM-u. Firmware węzła z własną stroną WWW zbudujesz samodzielnie dla ESP32-S3
(`pio run -e esp32s3-node`). Węzeł headless wypisuje diagnostykę magistrali na **konsolę USB**
(115200 Bd): liczniki ramek i ostrzeżenie „brak pytań od mastera", gdy nic nie odbiera.
**Wszystkie** kanały każdego węzła są lustrzane na masterze jako `xN_<id>`, gdzie `N` to
adres węzła (bez sztywnego limitu liczby kanałów ani węzłów), więc trafiają na pulpit,
do pierścienia LED, do Home Assistant, na wykresy i do plików CSV.

### Kilka węzłów na jednej magistrali

Master nie musi zgadywać, kto siedzi na magistrali — prowadzi **listę węzłów** (zakładka
**ESP i magistrala RS485** → *Lista węzłów*). Adres modułu można nadać na dwa sposoby:

- **Jedna uniwersalna binarka** (`…-node-c3-4mb.bin`) — adres fabryczny 2, a potem zmiana
  adresu przez magistralę (niżej). Wystarczy dla pojedynczego węzła.
- **Gotowe binarki z adresem** (`…-node-c3-4mb-addr3.bin` … `-addr8.bin`) — wgrane na **nowy**
  moduł C3 (pusta NVS) startują od razu na adresie 3…8, więc kilka świeżych modułów podłączonych
  jednocześnie **nie koliduje** na magistrali. Wszystkie buduje `tools\build_release.ps1`
  (flaga `-DSTACJA_NODE_ADDR=N`), a `-PublishNode` kopiuje je do `data\node\`, skąd stacja
  wydaje je w zakładce **ESP i magistrala RS485 → Pliki do wgrania**.

- **Dodaj** — wpisz adres i naciśnij *Dodaj*; master sprawdza, czy moduł odpowiada pod tym adresem
  (maks. `RS485_MAX_NODES` = 8 węzłów, adres w granicach 2–32).
- **Zmień adres** — podaj obecny i nowy adres; master przestawia adres **przez magistralę**
  (moduł zapisuje go w swojej NVS i restartuje się), więc nie trzeba go przeprogramowywać kablem.
- **Skan** — master sam przechodzi adresy 2–32 i dopisuje te, które odpowiedziały.
- Lista jest zapisywana w NVS mastera, a odpytywanie idzie **po kolei z odstępem min. 2 s** na węzeł,
  żeby wolniejsze moduły zdążyły odpowiedzieć.
- Każdy węzeł z listy dostaje w menu bocznym własne podmenu **ESP #2**, **ESP #3** … (kolejność listy),
  a na pulpicie sekcję o nazwie `ESP #n (Adres m)` — z własnymi pinami, kalibracją i sterownikami.
- W CSV (i w Home Assistant) nazwy kanałów niosą adres węzła: `x2_*` to węzeł o adresie 2 itd.
  Kolumny `x2_*` są w szerokim pliku `/logs/YYYY-MM.csv`; kanały pozostałych węzłów trafiają do
  dziennika długiego `/logs/extra-YYYY-MM.csv` (wykresy czytają je stamtąd).

### Podłączenie

| Master | Węzeł (ESP32-C3 SuperMini) | Uwagi |
|---|---|---|
| TX (GPIO 38) | RX (GPIO 20) | przewody łączymy „po kolei": 38–20 i 39–21, bez krzyżowania |
| RX (GPIO 39) | TX (GPIO 21) | piny zmienisz w zakładce **Piny** (funkcje `rs485_rx`, `rs485_tx`, `rs485_de`) |
| GND | GND | wspólna masa jest obowiązkowa — to trzeci przewód tej linii |
| DE/RE (GPIO 45) | DE/RE (GPIO 10) | **niepotrzebne** przy bezpośredniej linii TTL (ustaw `rs485_de = -1`); ma sens tylko z konwerterem MAX485/SP3485 |

W obecnym układzie **nie ma konwertera MAX485** — to bezpośrednia linia TTL (3 przewody:
dwa na dane + wspólny GND), więc **nie ma linii A/B ani terminacji 120 Ω**. Konwerter
MAX485/SP3485 z parą różnicową `A`/`B` (skrętka, na dłuższym kablu 120 Ω na obu końcach,
przy odwrotnym działaniu najczęściej wystarczy zamienić A z B) jest potrzebny dopiero wtedy,
gdy do magistrali dołączy prawdziwe urządzenie różnicowe np. czujnik Modbus RTU.

Domyślne piny mastera to **TX = GPIO 38, RX = GPIO 39** (`include/pins.h`), a węzła na C3
SuperMini **RX = GPIO 20, TX = GPIO 21** (UART0 tej płytki) — dzięki temu przy połączeniu
„po kolei" (38–20, 39–21) wszystko się zgadza. Nadrzędna zasada jest jednak zawsze ta sama:
**RX jednego ESP musi trafić na TX drugiego**. Gdy łączysz dwa ESP32-S3 (albo dwa C3),
przewody trzeba skrzyżować, bo oba końce mają ten sam domyślny układ pinów.

Sprawdzone na sprzęcie (2026-09-20): master (ESP32-S3, RX 39 / TX 38) + węzeł C3 SuperMini
(RX 20 / TX 21) połączone „po kolei" — magistrala zgłasza `online: true`, `node_count: 1`,
`latency_ms` ok. 190 ms, `err_crc: 0`, `mirrored: 169/179`, a kanały węzła pojawiają się
na pulpicie mastera w sekcji `ESP #2 (Adres 2)`. Dowód na żądanie:
`GET /api/rs485/action?a=sniff&pin=38` → `{"idle":100,"de_level":0,"self_tx":true,"drive":52}`
(linia wolna i pad TX faktycznie nadaje w przewód).

Zasilanie modułu RS485: **3V3 albo 5V** (zależnie od modułu), nigdy z obu źródeł naraz.
Oba urządzenia muszą mieć **tę samą prędkość** i **różne adresy** (fabrycznie master = 1,
węzeł = 2, 115200 b/s).

### Konfiguracja

Zakładka **ESP i magistrala RS485** → *Ustawienia magistrali*: włączenie magistrali, rola tego
urządzenia (master / węzeł), adresy, prędkość, interwał odpytywania, limit czasu odpowiedzi,
liczba powtórzeń oraz przełączniki *log węzła do CSV*, *wysyłka węzła do Home Assistant*
i *własna sieć Wi-Fi węzła*. **Zmiana roli działa po restarcie stacji** (od niej zależy cały
start: Wi-Fi, MQTT, OTA); zmiana prędkości i pinów działa od razu.

Role są symetryczne: **węzeł** uruchamia się bez łączenia z internetem (tylko pomiary,
magistrala i opcjonalny punkt dostępowy), a **master** obsługuje pulpit, publikację MQTT,
wykresy i zapis CSV.

### Płytka węzła — automatyczne piny i gotowe pliki

Master nie musi wiedzieć ręcznie, gdzie węzeł ma podłączone czujniki: zna **katalog płytek**
([src/board.cpp](src/board.cpp)) i dla każdej z nich ma **domyślne piny, dopuszczalny numer GPIO,
rodzaj ADC i gotowe pliki do wgrania**. W zakładce **ESP i magistrala RS485** wybierz w polu
*Płytka węzła* jedną z opcji:

| Płytka | Chip | Flash | PSRAM | Uwagi |
|---|---|---|---|---|
| ESP32-S3 WROOM-1 N16R8 | esp32s3 | 16 MB | 8 MB octal | pełny zestaw funkcji |
| ESP32-S3 N8R8 | esp32s3 | 8 MB | 8 MB octal | pełny zestaw funkcji |
| ESP32-S3 N8R4 / N8R2 | esp32s3 | 8 MB | 4 / 2 MB quad | pełny zestaw funkcji |
| ESP32-C3 SuperMini | esp32c3 | 4 MB | brak | mniej GPIO, bez karty SD, bez PMS5003 |
| ESP32-C6 (bez pliku) | esp32c6 | 4 MB | brak | wymaga Arduino core 3.x — obecnie tylko opis pinów |

Co się dzieje po wyborze płytki:

1. **Piny uzupełniają się same** — tabela *Piny węzła* dostaje domyślne GPIO tej płytki
   (możesz je poprawić i wysłać przyciskiem *Zapisz piny węzła*). Gdy węzeł odpowiada, stacja
   czyta jego piny na żywo i pokazuje źródło: *odczytane z węzła* albo *szablon płytki*.
2. **Pole „Zgodność z węzłem”** ostrzega, gdy wybrana płytka nie zgadza się z tą, którą zgłasza
   węzeł (płytka jest wysyłana razem z resztą informacji w ramce RS485).
3. **Tabela „Pliki do wgrania”** pokazuje pliki wgrane do pamięci stacji (`-PublishNode`) wraz
   z rozmiarem i sposobem wgrania; pobiera je trasa `/api/boards` → `/node/<płytka>.bin`.
   Dla węzła C3 dodatkowo wypisuje binarki `c3mini-addr3.bin` … `c3mini-addr8.bin`
   (adres fabryczny 3…8), jeśli są wgrane (`-PublishNode`).
4. **Wybór jest zapamiętywany w przeglądarce** (`localStorage`), a **nie w NVS stacji** — zmiana
   płytki nie kasuje i nie nadpisuje żadnych ustawień.

Katalog płyt odczytasz też z API: `GET /api/boards` (lista płytek, ich piny, wykryta płytka tej
stacji oraz informacja, które obrazy są dostępne).

Domyślne piny węzła **ESP32-C3 SuperMini** to zestaw pod czujniki wiatru: **I²C SDA = GPIO 8,
SCL = GPIO 9** (wiatrowskaz **AS5600** pod `0x36`, a obok niego BME280/SHT4x i reszta magistrali),
**anemometr = GPIO 5** (kontaktron albo czujnik Hall), **OneWire = GPIO 4**, **sonda gleby = GPIO 1**,
**pierścień RGB = GPIO 2**, **RS485 RX 20 / TX 21 / DE 10**. Wyłączone są: deszczomierz, wiatrowskaz
analogowy (kierunek daje AS5600), LED statusu i przycisk BOOT — te dwa ostatnie dlatego, że GPIO 8
i 9 SuperMini ma wyprowadzone właśnie na diodę i przycisk, a jeden pin może mieć tylko jedną funkcję.
Deszczomierz przeznaczony jest na **osobny węzeł** (kolejna płytka C3 na tej samej magistrali).
Pełna lista z legendą pinów: [docs/CZUJNIKI.md](docs/CZUJNIKI.md).

### Protokół

```
AA 55 | ver | dst | src | typ | seq(2) | len(2) | payload(JSON, len) | crc16(2)
  ver = 0x01, typ = 0x01 zapytanie / 0x81 odpowiedź
  crc16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) z pól ver..payload
zapytanie:  {"c":"sync"}
odpowiedź:  {"ok":true,"err":"","data":{ ... }}
```

Maksymalna długość JSON-a w ramce: **8192 B** (`RS485_MAX_PAYLOAD`). Magistrala używa
**UART0** (konsole obsługuje natywne USB, UART1 zajmuje GPS, UART2 — czujnik pyłu).

Dostępne polecenia węzła:
`ping`, `hello`, `sync`, `status`, `diag`, `chan`, `cfg`, `pins`, `log` (odczyt) oraz
`pins_set`, `pins_reset`, `cfg_set`, `chan_set`, `log_clear`, `time` i
`action` (`do: reboot` / `do: discover`) — zapis i akcje sterowane z zakładki
**ESP i magistrala RS485** na masterze. Szczegóły interfejsu opisuje [src/rs485.h](src/rs485.h).
Polecenie `status` zwraca **pełny** stan strony WWW (kilkadziesiąt kB), więc nie mieści się
w jednej ramce — do podglądu węzła służą `sync` (skrócony stan, ~6 kB) i `diag` (~3,5 kB).

### REST API

| Trasa | Opis |
|---|---|
| `GET /api/rs485` | stan magistrali (rola, adresy, statystyki ramek, lista węzłów `nodes[]`, `active_addr`, informacje o węźle) |
| `POST /api/rs485` | zapis ustawień magistrali (`{ok:true,restart:…,state:…}`) |
| `GET` + `POST` `/api/rs485/action?a=…` | `poll`, `scan`, `selftest`, `refresh`, `time_sync`, `discover_peer`, `reboot_peer`, `clear_peer_log`, `pins_reset`, `restart` (przebudowa portu magistrali) oraz diagnostyka `sniff`/`scout` (`&pin=38`, `&pins=38,47,21`); dodatkowo `add`/`del`/`assign` z parametrem `addr` (lista węzłów) |
| `GET` + `POST` `/api/node?c=…` | przezroczysty dostęp do węzła: `?addr=<adres>` albo `?i=<pozycja na liście>`, domyślnie węzeł aktywny |
| `GET /api/boards` | katalog płytek dla węzła: wykryta płytka tej stacji, domyślne piny, limity GPIO i dostępne obrazy |

### Rozwiązywanie problemów

- **Węzeł nie odpowiada** — w obecnym układzie to linia TTL 3-przewodowa: `master TX 38 → RX węzła 20`,
  `master RX 39 → TX węzła 21` i **wspólny GND** (bez konwertera MAX485). Sprawdź, czy przewody
  nie są połączone „TX do TX"/„RX do RX", czy jest wspólna masa, zgodność prędkości i różne adresy.
  Statystyki w karcie *ESP i magistrala RS485* pokażą, czy ramki wychodzą
  (`Zapytania wysłane`) i czy wracają (`Odpowiedzi / Błędne CRC / Błędy nagłówka / Brak odpowiedzi`).
  Gdy łączność nie wraca, użyj `GET /api/rs485/action?a=sniff&pin=38` (`idle: 100` + `drive` ≈ 50 %
  = pad TX nadaje w przewód), `?a=sniff&pin=39` (nasłuch pinu RX + liczniki `link_*`) i `?a=restart`
  (przebudowa portu magistrali bez restartu stacji).
- **Test bez magistrali** — przycisk *Test protokołu* sprawdza w pamięci budowę ramki, CRC,
  odpowiedź na `ping` i odrzucanie uszkodzonej ramki; nie wymaga podłączonego węzła.
- **Skan magistrali** — `scan` wysyła `ping` pod adresy 2–32 (`RS485_SCAN_MAX_ADDR`) i wypisuje te,
  które odpowiedziały. W tle master robi to samo po dwa adresy na cykl, żeby nie blokować pętli.
- **Węzeł na C3 nic nie odbiera** — podłącz konsolę USB węzła (115200 Bd) i wgraj firmware
  węzła z `-DSTACJA_HEADLESS=1`: pojawia się licznik ramek i ostrzeżenie „brak pytań od mastera",
  gdy węzeł nie słyszy mastera. Wtedy sprawdź, czy RX/TX nie są połączone „RX do RX"
  (dwa ESP32-S3 zawsze wymagają skrzyżowania), a na masterze porównaj faktyczne piny magistrali
  z zakładką **Piny** (`GET /api/rs485` zwraca `rx`, `tx`, `de`).
- **Magistrala wyłączona** — trasy `/api/node` zwracają wtedy `400 {"error":"Magistrala RS485 jest wyłączona"}`.

> **Uwaga dla rozbudowy API:** `ESPAsyncWebServer` dopasowuje URI **prefiksowo** — trasa
> `/api/rs485` obsłuży także `/api/rs485/action`. Dlatego trasy szczegółowe rejestrujemy
> **przed** trasami nadrzędnymi (`/api/rs485/action` przed `/api/rs485`, `/api/forecast/analysis`
> przed `/api/forecast`, `/api/pins/reset` przed `/api/pins`). Ten sam efekt daje prefiks
> z gwiazdką (`/api/rs485/*`), ale kolejność jest prostsza w utrzymaniu.

## Obejście błędu odczytu flash (ESP32-S3) — ważne

Na tej płytce odczyt flash z wyłączonym cache (LittleFS, NVS, weryfikacja obrazu OTA) zwracał
błędne dane: przy transferze ≥ 64 bajtów górna połowa każdej porcji 64-bajtowej była kopią dolnej,
przez co littlefs zgłaszał `Corrupted dir pair at {0x0, 0x1}`, system plików nie dawał się
zamontować, a strona WWW była pusta. Odczyty ≤ 32 bajtów oraz zapis działały bezbłędnie.

Rozwiązanie: [src/flashfix.cpp](src/flashfix.cpp) podmienia w czasie startu funkcję `read` w tablicy
sterownika flash (kopia tablicy w DRAM, kod w IRAM) i dzieli każde żądanie na porcje 32-bajtowe.
`installFlashReadFix()` jest wołane na początku `setup()`, przed `LittleFS.begin()`, więc obejmuje
wszystkie ścieżki odczytu. Koszt: kilka dodatkowych transferów na jedno żądanie odczytu.

- Potwierdzenie w logu startowym: `Obejście odczytu flash aktywne (porcje 32 B)`.
- Kontrola zawartości strony: linia `Strona www: index.html <bajty> B, suma kontrolna <FNV-1a>` —
  suma FNV-1a tego samego pliku z katalogu `data/` musi być identyczna.
- Nie wymaga zmian w `platformio.ini`; sprawdzone dla `memory_type = qio_opi` i `dio_opi`.

## Przyszłe moduły

- **Czujnik jakości powietrza (PMS5003)** — obsługa UART już jest; podłącz TX/RX jak w pins.h.
- **Kamera** — piny do ustalenia po doborze modułu (OV2640/OV5640). Wtedy przełącz SD na SPI,
  a GPIO 40–42 zostaną zwolnione dla kamery.

## Autor i licencja

- **Autor i właściciel praw autorskich:** kruzio1985 — [github.com/kruzio1985](https://github.com/kruzio1985)
- **Kontakt:** kruzio1985@users.noreply.github.com
- **Licencja:** [PolyForm Noncommercial License 1.0.0](LICENSE) — kod, strona www i dokumentacja
  mogą być używane, kopiowane i zmieniane **tylko niekomercyjnie**, z zachowaniem informacji
  o autorze (w tym linku do tego repozytorium w pracach pochodnych).
  **Użytek komercyjny wymaga pisemnej zgody autora.**
- **Pełny tekst licencji na stacji:** zakładka **Informacje** → przycisk *Pełny tekst licencji*
  (plik serwowany jako `/LICENSE.txt` — LittleFS rozróżnia wielkość liter) albo w repozytorium
  [data/LICENSE.txt](data/LICENSE.txt).

Nagłówek z informacją o autorze i licencji znajduje się w każdym pliku źródłowym projektu
(`src/*`, `include/*`, `data/index.html`, `data/i18n.js`, `data/public.html`, `platformio.ini`,
`partitions.csv`, README). Przy dopisywaniu nowego pliku warto skopiować ten nagłówek.

> **Uwaga:** pliki [LICENSE](LICENSE) i [data/LICENSE.txt](data/LICENSE.txt) muszą mieć
> **identyczną treść** — pierwszy jest wersją w repozytorium, drugi jest wgrywany na stację
> (`uploadfs`) i wyświetlany w przeglądarce. Po zmianie jednego skopiuj go na drugi:

```bash
cp LICENSE data/LICENSE.txt      # Linux/macOS
copy LICENSE data\LICENSE.txt    # Windows
```

Zakładka **Informacje** na stronie www (PL/EN/DE) pokazuje też dane techniczne stacji
(wersja firmware, data kompilacji, model płytki, adres IP, czas pracy, wolna pamięć RAM/PSRAM)
oraz listę wykorzystanych bibliotek wraz z ich wersjami i licencjami.

### Biblioteki firm trzecich

| Biblioteka | Wersja | Licencja |
|---|---|---|
| ArduinoJson | 7.4.3 | MIT |
| ESPAsyncWebServer | 3.6.0 | LGPL-3.0 |
| AsyncTCP | 3.3.2 | LGPL-3.0 |
| PubSubClient | 2.8 | MIT |
| Adafruit BME280 Library | 2.3.0 | BSD |
| Adafruit Unified Sensor | 1.1.15 | Apache-2.0 |
| Adafruit BusIO | 1.17.4 | MIT |
| Adafruit NeoPixel | 1.15.5 | LGPL-3.0 |
| OneWire | 2.3.8 | MIT |
| DallasTemperature | 3.11.0 | LGPL-2.1 |
| Arduino-ESP32 (rdzeń) | 3.x | LGPL-2.1 |
| ESP-IDF | 5.x | Apache-2.0 |

Licencja PolyForm Noncommercial dotyczy **tylko kodu tego projektu**. Każda biblioteka ma własne warunki
(linki i licencje są wypisane w zakładce *Informacje*), a firmware zbudowany z tych bibliotek
objęty jest również ich postanowieniami.

## Struktura projektu

```
platformio.ini      — konfiguracja budowania (PSRAM octal, flash 16 MB, LittleFS, OTA)
partitions.csv      — partycje 16 MB: nvs, otadata, app0, app1, LittleFS (~9,94 MB na WWW)
include/pins.h      — mapa pinów
src/                — kod źródłowy
  config.*          — ustawienia (NVS + kopia /config.json na LittleFS)
  nvs_store.*       — zapis/odczyt ustawień w partycji NVS (przetrwa aktualizację)
  sensors.*         — BME280, BH1750, DS18B20, deszcz, wiatr, PMS5003, SCD4x/SGP30 (strefy kanałów)
  sensors_extra.cpp — SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA, AS5600, ADS1115, SEN5x/SPS30
  drv_i2c.*         — sterowniki I²C: SHT4x, BMP581, VEML7700, LTR-390UV, MMC5983MA, AS5600, ADS1115
  drv_pm.*          — sterowniki pyłu na I²C: Sensirion SEN5x i SPS30 (autodetekcja)
  i2c_util.h        — helpers I²C (odczyt/zapis 8- i 16-bit, CRC)
  led_ring.*        — pierścień WS2812B: kolory stanu pogody, jasność, limit prądu
  weather_services.*— wysyłka do zewnętrznych serwisów pogodowych (zadanie FreeRTOS)
  mqtt_client.*     — MQTT + discovery Home Assistant
  sd_card.*         — karta microSD (SDMMC 1-bit / SPI) + formatowanie
  logger.*          — zapis CSV na kartę SD
  rs485.*           — magistrala RS485: protokół ramek, master (odpytywanie) i węzeł
  board.*           — katalog płytek węzła: domyślne piny, limity GPIO, gotowe pliki
  web_server.*      — REST API + strona WWW
  ota.*             — ArduinoOTA + aktualizacja z przeglądarki
  syslog.*          — bufor logów w PSRAM + diagnostyka
  flashfix.*        — obejście błędu odczytu flash (porcje 32-bajtowe)
  main.cpp          — start, Wi-Fi, pętla główna
tools/analyze_i18n.js — kontrola pokrycia tłumaczeń PL/EN/DE
data/index.html     — interfejs WWW (SPA, menu boczne z podmenu: struktura MENU)
data/i18n.js        — słownik PL → EN/DE (napis źródłowy jako klucz)
data/public.html    — publiczna strona z danymi i reklamą firmy
data/node/          — obrazy węzłów (powstają z -PublishNode; serwowane jako /node/…)
LICENSE             — licencja projektu (PolyForm Noncommercial 1.0.0, autor: Kruzio)
data/LICENSE.txt    — kopia licencji wgrywana na stację (serwowana jako /LICENSE.txt)
docs/CZUJNIKI.md    — legenda czujników: podłączenie, parametry, kalibracja, wykresy, logi
```

## Partycje (16 MB)

| Nazwa | Offset | Rozmiar | Zastosowanie |
|---|---|---|---|
| nvs | 0x009000 | 20 kB | ustawienia stacji (Wi-Fi, piny, kalibracja, LED) + dane Wi-Fi IDF — przetrwa OTA i uploadfs |
| otadata | 0x00E000 | 8 kB | wskaźnik aktywnej partycji aplikacji |
| app0 | 0x010000 | 3 MB | firmware (slot A) |
| app1 | 0x310000 | 3 MB | firmware (slot B, OTA) |
| spiffs | 0x610000 | 9,94 MB | LittleFS — strona WWW |

### Partycje (8 MB i 4 MB)

Węzeł na mniejszym module ma własną tablicę partycji (mniejsze sloty OTA i mniejszy LittleFS):

| Plik | Partycja | Offset | Rozmiar | Zastosowanie |
|---|---|---|---|---|
| `partitions-8mb.csv` | app0 / app1 | 0x010000 / 0x310000 | 3 MB / 3 MB | firmware węzła (8 MB, sloty OTA) |
| `partitions-8mb.csv` | spiffs | 0x610000 | 1,88 MB | strona WWW węzła |
| `partitions-4mb.csv` | app0 / app1 | 0x010000 / 0x1B0000 | 1,63 MB / 1,63 MB | firmware węzła (4 MB, C3 SuperMini) |
| `partitions-4mb.csv` | spiffs | 0x350000 | 704 kB | system plików węzła (C3 nie serwuje strony WWW) |

> Obrazy LittleFS dla węzłów powstają **bez** katalogu `data\node\` (binarki węzłów wydaje
> tylko master przez `/api/boards`) — pilnuje tego `tools/stage_node_fs.py` podłączony jako
> `extra_scripts` środowisk węzła, a `tools/build_release.ps1` stosuje to samo dla `…-www-8mb.bin`.

> Na ESP32-C3 (4 MB) firmware węzła zajmuje ~42% slotu aplikacji; jego partycja LittleFS
> (704 kB) nie jest używana na stronę WWW, bo węzeł C3 pracuje bez sieci i trzyma tam
> tylko kopię ustawień.
