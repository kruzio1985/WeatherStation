# 🌦️ Stacja Pogody — legenda czujników i sterowników

Dokument opisuje **każdy** czujnik, który można podłączyć do stacji (ESP32-S3 WROOM-1 N16R8),
wraz z informacją:

| Kolumna w tabelach | Znaczenie |
|---|---|
| **Czujnik** | model / rodzina układów |
| **Co daje** | co mierzy i do czego służy w stacji |
| **Podłączenie** | magistrala, pin stacji, adres I²C |
| **Parametry** | zakres, dokładność, zasilanie |
| **Kalibracja** | jak i czym kalibrować |
| **Wykres / log** | identyfikator w zakładce *Wykresy* i kolumna w CSV |

Legenda statusu: ✅ **działa w kodzie** · 🟡 **sterownik gotowy do dopisania / planowany** · ⛔ **nieobsługiwane**

Na stronie www legenda pokazuje domyślnie tylko czujniki ze statusem ✅ (czyli te, których
sterowniki są w firmware stacji) — pozycje 🟡 i ⛔ odsłania przełącznik
*Pokaż też czujniki bez sterownika*. Tabele **Czujniki** i **Kalibracja** pokazują wyłącznie
sprzęt, który odpowiedział na magistrali; kanały zapisane w konfiguracji, ale niepodłączone,
odsłania przełącznik *Pokaż też nieaktywne*.

Zakładka *Piny* pokazuje **wszystkie piny płytki w kolejności fizycznej** — najpierw lewa
kolumna złącza (J1, od 3V3 do GND), potem prawa (J3, od GND do GND). Przy każdym GPIO jest
lista funkcji do wyboru (opis wybranej funkcji pojawia się obok), więc przypisanie zmienia się
bez ręcznego wpisywania numerów. Lista zależy od wybranej płytki (S3 N16R8/N8R4, C3 SuperMini).

Piny domyślne i ich nazwy (edycja w zakładce *Piny*):

```
I²C      SDA = GPIO 8    SCL = GPIO 9        (100 kHz, pull-up 4,7 kΩ do 3V3)
         │ BME280 0x76/77 · BH1750 0x23 · SCD4x 0x62 · SGP30 0x58 · AS3935 0x03
         │ SHT4x 0x44…0x46 · BMP581 0x46/47 · VEML7700 0x10 · LTR-390UV 0x53
         │ MMC5983MA 0x30/31 · AS5600 0x36 · ADS1115 0x48…0x4B · SEN5x/SPS30 0x69
1-Wire   GPIO 4 (DS18B20 ×8)
DESZCZ   GPIO 5 (przerwanie, reed / przechyłowy)
WIATR    GPIO 6 (impulsy anemometru)
KIERUNEK GPIO 1 (ADC1, potencjometr) — AS5600 zastępuje wiatrowskaz na I²C
GLEBA    GPIO 2 (ADC1, sonda pojemnościowa / rezystancyjna)
PYRANO   GPIO −1 = wyłączony (włącz pin ADC albo wejście ADS1115)
PM       RX = GPIO 18, TX = GPIO 17  (UART2, 9600 baud)
GPS      RX = GPIO 15, TX = GPIO 16  (UART1, 9600 baud, NMEA 0183)
RS485    RX = GPIO 39, TX = GPIO 38, DE = GPIO 45  (UART0, moduł pełni rolę węzła;
         węzeł C3 SuperMini łączymy po kolei: 38 → RX 20, 39 → TX 21)
BURZA    AS3935: przerwanie GPIO 14 + I²C 0x03
SD/MMC   CLK 40 · CMD 41 · D0 42
SD/SPI   CS 10 · SCK 12 · MOSI 11 · MISO 13
LED RGB  GPIO 7 (WS2812B, 36 px)
DIODA    GPIO 21 (LED statusu) · PRZYCISK GPIO 0 (BOOT, tryb AP / reset)
```

Piny domyślne **węzła ESP32-C3 SuperMini** („ESP #2", zakładka *ESP i magistrala RS485* →
*Piny węzła*) — domyślnie node na wiatr, bez deszczomierza:

```
I²C      SDA = GPIO 8    SCL = GPIO 9        (AS5600 0x36, BME280, SHT4x, ...)
1-Wire   GPIO 4 (DS18B20 ×8)
WIATR    GPIO 5 (impulsy anemometru: kontaktron / czujnik Hall)
DESZCZ   GPIO −1 = wyłączony (deszczomierz idzie na osobny węzeł)
KIERUNEK GPIO −1 = wyłączony (kierunek daje AS5600 na I²C; pin włączony = wiatrowskaz analogowy)
GLEBA    GPIO 1 (ADC1, sonda pojemnościowa / rezystancyjna)
LED RGB  GPIO 2 (WS2812B)
DIODA    GPIO −1 = wyłączony · PRZYCISK GPIO −1 = wyłączony (GPIO 8/9 zajmuje I²C)
RS485    RX = GPIO 20, TX = GPIO 21, DE = GPIO 10
         │ stacja: RX 39 ← TX 21 węzła · TX 38 → RX 20 węzła · GND-GND (wspólna masa)
         │ bezpośrednia linia TTL (bez MAX485); przy dłuższym kablu skrętka + ekran do GND
         │ z jednej strony, a konwerter MAX485/terminacja 120 Ω dopiero dla Modbus
Karta SD, GPS, PMS5003, AS3935, pyranometr, radar, DHT, HX711, I²S: GPIO −1 (brak pinów na C3)
```

> **Wolne GPIO węzła C3:** 0, 3, 6, 7 (piny 18/19 obsługują tylko natywne USB, więc firmware
> ich nie używa). Piny 2, 8, 9 to piny strappingu — działają, ale przy starcie nie mogą być
> ściągnięte do masy.

> **Sterowniki dodatkowe na I²C** (te same piny SDA/SCL, wykrywane przy starcie) opisano
> w rozdziałach 1–14 i 32. Wszystkie działają z magistralą 100 kHz i nie kolidują
> adresami z BME280 (0x76/77) ani BH1750 (0x23).

> **Zasilanie:** wszystkie czujniki 3,3 V. 5 V (pin `5V` płytki) tylko dla: PMSxxx, SPS30,
> przekaźników, wentylatora obudowy, paska LED. GND czujników i GND płytki muszą być wspólne.
> Linie I²C dłuższe niż ~30 cm wymagają pull-upów 2,2 kΩ i skrętki z GND.

> **Czujniki zewnętrzne po RS485:** węzeł (np. ESP32-C3 SuperMini) może stać na zewnątrz i wysyłać
> swoje pomiary do stacji po skrętce. Kanały węzłów pojawiają się na pulpicie, wykresach,
> w CSV i w Home Assistant, a kalibruje się je w zakładce *ESP i magistrala RS485*.
> Podłączenie, protokół i przykłady: [README → Węzły po RS485](../README.md#węzły-po-rs485-czujniki-zewnętrzne).

---

## 1. Temperatura powietrza

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **BME280** ✅ | temperatura, wilgotność, ciśnienie — bazowy czujnik stacji | I²C `0x76` (0x77) | −40…+85 °C, ±0,5 °C, 1,71…3,6 V | offset w zakładce *Kalibracja*; termometr referencyjny | `temp` → kolumna `temp` |
| **BME680 / BME688** 🟡 | T + RH + ciśnienie + gaz (TVOC/IAQ) | I²C `0x76`/`0x77` | −40…+85 °C, ±0,5 °C; gaz: 0…500 kΩ | jak wyżej + 48 h wygrzewania czujnika gazu | `temp`, `press`, `tvoc` |
| **SHT30 / SHT31 / SHT35** ✅ | dokładniejsza T/RH (SHT35 ±0,1 °C) | I²C `0x44`/`0x45` | −40…+125 °C, 2,4…5,5 V | offset; wzorzec solny dla RH | `temp`, `hum` |
| **SHT40 / SHT41 / SHT45** ✅ | T/RH nowej generacji, mały pobór | I²C `0x44`/`0x45`/`0x46` | −40…+125 °C, ±0,1 °C (SHT45) | jak SHT3x; offset kanału w *Kalibracja* | `sht_t` / `sht_h` → kolumny `sht_t`, `sht_h` |
| **AHT10 / AHT20 / AHT21** ✅ | tani T/RH | I²C `0x38` | −40…+85 °C, ±0,3 °C | offset, RH × korekta liniowa | `temp`, `hum` |
| **HDC1080 / HDC2010 / HDC3020** 🟡 | T/RH niski pobór (bateria) | I²C `0x40`/`0x41` | −40…+125 °C, ±0,2 °C | offset | `temp`, `hum` |
| **TMP117 / MCP9808 / LM75 / Si7021** 🟡 | precyzyjna T (TMP117 ±0,1 °C, wzorzec NIST) | I²C `0x48…0x4F` | −55…+125 °C | wzorcowanie jednopunktowe w lodzie | `temp` |
| **DS18B20 ×N** ✅ | temperatura w wielu punktach (gleba, woda, grunt) | 1-Wire `GPIO 4` | −55…+125 °C, ±0,5 °C | offset per kanał (`DS18B20 #n`) | `ds_0…ds_7` → `ds0…ds7` |
| **DS18B20 w wersji wodoodpornej** ✅ | j.w. na kablu 1–10 m | 1-Wire | j.w., kabel silikonowy | pomiar w wodzie lodowej (0 °C) | `ds_*` |
| **PT100 / PT1000** 🟡 | temperatura wzorcowa, dokładna (±0,1…0,3 °C) | MAX31865 SPI (`CS 15`, `SCK 12`, `MOSI 11`, `MISO 13`) | −200…+600 °C | 2/3/4-przewodowo + rezystor odniesienia (`Rref`) w konfiguracji | `temp` (kanał zewnętrzny) |
| **Termopary K / J / T (+ MAX31855 / MAX6675)** ✅ | bardzo wysokie temperatury (komin, piec) | SPI, `CS 16` | −200…+1350 °C (K) | kompensacja zimnych końców, offset | `temp` |
| **NTC 10 k / 100 k** 🟡 | tani pomiar T (sonda glebowa/wodna) | ADC1: `GPIO 1..3`, `GPIO 14` | −40…+125 °C | tabela Steinharta–Harta (B25/85), 3 punkty | `temp` |
| **MLX90614 / 90615** ✅ | temperatura bezdotykowa (niebo, chmury, grunt) | I²C `0x5A` | −70…+380 °C, ±0,5 °C | emisyjność (`mlx_emiss` w *Kalibracja*) + offset | `mot_ir_obj_t`, `mot_ir_amb_t` |
| **MLX90632 / MLX90640 / MLX90641** 🟡 | termowizja punktowa / matryca 32×24 | I²C `0x3A`/`0x33` | −40…+300 °C | jak wyżej, korekta tła (Ta) | `temp`, mapa w zakładce *Diagnostyka* |
| **Si7051 / Si7060** 🟡 | T ultra-niski pobór | I²C `0x40` | −40…+125 °C, ±0,1 °C | offset | `temp` |

## 2. Wilgotność powietrza

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **BME280 / BME680 / BME688** ✅🟡 | RH bazowa | I²C | 0…100 % RH, ±3 % | sól: NaCl 75 %, MgCl₂ 33 %, K₂CO₃ 43 % | `hum` |
| **SHT3x / SHT4x / AHT2x / HDC2xxx** ✅🟡 | alternatywa dla T/RH: SHT4x działa, SHT3x/AHT/HDC planowane (szczegóły — rozdział 1) | I²C | ±1,5…2 % RH | komora solna, 2 punkty → offset + nachylenie | `hum` (SHT4x: `sht_h`) |
| **DHT11 / DHT22 / AM2301 / AM2320** ✅ | tanie T/RH (DHT11 tylko ±5 % RH, 1 Hz) | 1-Wire-like `GPIO 42` (+ 4,7 kΩ do 3V3) | DHT22: 0…100 % RH, ±2 % | offset, wymaga 2 s odstępu między odczytami | `temp`, `hum` |
| **SHT85 / SHT75 (sonda kablowa)** 🟡 | RH w kanale pomiarowym stacji | I²C / napięciowy | ±1,5 % RH | jak SHT3x | `hum` |
| **Czujnik punktu rosy (obliczany)** ✅ | dew point z T i RH (Magnus) | — | z `temp` + `hum` | pośrednio przez kalibrację T i RH | `dew` (MQTT), `hum` w CSV |

## 3. Ciśnienie atmosferyczne

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **BME280 / BME680** ✅🟡 | ciśnienie absolutne | I²C | 300…1100 hPa, ±1 hPa | offset względem stacji meteo / wysokość n.p.m. | `press` |
| **BMP180 / BMP280 / BMP380** 🟡 | ciśnienie (BMP380 ±0,03 hPa) | I²C `0x76`/`0x77` | 300…1100 hPa | offset + `altitude()` w kodzie | `press` |
| **BMP388 / BMP390** ✅ | najwyższa rozdzielczość | I²C/SPI `0x76` | 300…1250 hPa, ±0,02 hPa | offset, kompensacja temperatury | `press` |
| **BMP581** ✅ | ciśnienie nowej generacji, ±0,06 hPa | I²C `0x46`/`0x47` (nie koliduje z BME280 0x76/77) | 300…1250 hPa | offset; tryb `continuous`, ODR 2 | `bmp_p` / `bmp_t` → kolumny `bmp_p`, `bmp_t` |
| **DPS310 / DPS368 / LPS22HH / LPS28 / MPL3115A2 / MS5611** ✅ | ciśnienie/kierunek trendu (burza) | I²C `0x77`/`0x5C`/`0x76` | ±0,1…0,5 hPa | offset, filtr IIR | `press` |
| **Ciśnienie na poziom morza** ✅ | MSLP do raportów i MQTT | — | z `press` + wysokości stacji | podaj wysokość n.p.m. w *Konfiguracja* | kanał „Ciśnienie (MSLP)” w MQTT |

## 4. Opad deszczu

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Deszczomierz przechyłowy (tipping bucket, 0,2794 mm/tyk)** ✅ | sumaryczny opad i intensywność | `GPIO 5` (przerwanie) | 0…100 mm/h, 0,2794 mm/impuls | wpisz `mm/impuls` (pole *Kalibracja → opad*), test: 100 ml przez lejek | `rain` |
| **Czujnik Hall / kontaktron + magnes** ✅ | impulsy przechyłki | `GPIO 5` | 3,3 V, prąd <1 mA | jak wyżej, filtr drgań 100 ms | `rain` |
| **FC-37 / YL-83 / LM393 (płytka rezystancyjna)** 🟡 | „pada / nie pada” + wilgotność | ADC1 `GPIO 2` | 0…3,3 V | próg wilgotności w kodzie (`obwód suchy → 0 mm`) | `rain` (binarnie) |
| **Deszczomierz optyczny / podczerwieniowy** 🟡 | opad bez części ruchomych | I²C / impuls | rozdzielczość 0,1 mm | kalibracja fabryczna + offset | `rain` |
| **Deszczomierz wagowy (load cell + HX711)** ✅ | opad z najwyższą dokładnością | HX711: `GPIO 21/47`, 5 V | 0,1 mm, 10 Hz | tara przy zerze, współczynnik g/mm | `rain` |
| **Liściowy — mokrość liścia (leaf wetness)** 🟡 | ryzyko chorób grzybowych roślin | ADC1 `GPIO 14` | 0…100 % | dwa punkty: sucho/pod wodą | `leaf` (MQTT + CSV) |

## 5. Prędkość wiatru

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Anemometr obrotowy (3/4-cup, impulsowy)** ✅ | prędkość wiatru i porywy | `GPIO 6` (impulsy) | 0…50 m/s, ±3 % | współczynnik `km/h na Hz` (domyślnie 2,4) w *Kalibracja* | `wind` (+ poryw w MQTT) |
| **Anemometr z kontaktronem / Hall (A3144)** ✅ | j.w. | `GPIO 6` | 3,3 V | jak wyżej + 1 s okno uśredniania | `wind` |
| **Anemometr ultradźwiękowy (bez części ruchomych)** 🟡 | prędkość + kierunek (kombajn) | UART/RS485 lub I²C | 0…60 m/s, ±0,3 m/s | kalibracja fabryczna, offset w kodzie | `wind`, `vane` |
| **Czujnik różnicy ciśnień (Pitot: MPXV7002, SDP810)** 🟡 | prędkość z ciśnienia dynamicznego | ADC1 / I²C `0x25` | 0…30 m/s | współczynnik rurki Pitota | `wind` |
| **Wiatromierz ręczny / stacja komercyjna (Davis)** 🟡 | źródło referencyjne | RS485 / 1-Wire | — | porównanie z anemometrem własnym | `wind` |

## 6. Kierunek wiatru

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Potencjometr / czujnik analogowy (vane)** ✅ | kierunek 0…360° | ADC1 `GPIO 1` | 0…3,3 V → 0…360° | tabela 8/16 kierunków → offset + zapis krzywej napięć | `vane` (+ `compass` w `/api/status`) |
| **AS5600 / AS5601** ✅ | magnetyczny enkoder absolutny 12-bit | I²C `0x36` | 0…360°, 0,088° | offset zerowania (`ZPOS`); włącza się tylko, gdy pin wiatrowskazu = −1 | `vane` → kolumna `vane` (+ MQTT) |
| **AS5048A / AS5048B / AS5047P** 🟡 | enkoder magnetyczny precyzyjny | SPI / I²C `0x40` | 14-bit, 0,022° | kalibracja zera i magnesu | `vane` |
| **MT6701 / MA730 / MA732 / TLE5012B** 🟡 | enkoder magnetyczny (SSI/ABZ/PWM) | SPI/SSI | 14–15-bit | jak wyżej | `vane` |
## 7. Magnetometry / kompas (kierunek wiatru bez vane’a)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **QMC5883L** ✅ | kompas 3-osiowy | I²C `0x0D` | ±1…8 G, 2 mG | kalibracja twarda/miękka (obrót 360°) + deklinacja `mag_decl` | `mot_heading`, `mot_mag_x/y/z` |
| **HMC5883L / HMC5983** 🟡 | j.w. | I²C `0x1E` | ±0,75…8 G | j.w. | `mot_heading`, `mot_mag_x/y/z` |
| **LIS3MDL / LIS2MDL** 🟡 | kompas niski pobór | I²C `0x1C`/`0x1E` | ±4…16 G | j.w. + temperatura pracy | `vane` |
| **MMC5983MA** ✅ | kompas bardzo dokładny (18-bit), azymut do kanału kierunku | I²C `0x30`/`0x31` | ±8 G, 0,4 mG | deklinacja magnetyczna `mag_decl` (NVS, *Kalibracja*) | `mmc_hdg` → kolumna `mmc_hdg` |
| **MMC5603** 🟡 | kompas 20-bit, tańszy | I²C `0x30` | ±8 G | jak wyżej | `vane` |
| **BMM150 / BMM350** 🟡 | kompas mały (kompatybilny z BMI270) | I²C `0x10`/`0x14` | ±1300 µT | j.w. | `vane` |
| **AK09918 / AK8963 (w MPU-9250)** 🟡 | kompas w IMU | I²C `0x0C` | ±4900 µT | j.w. | `vane` |
| **Korekcja deklinacji magnetycznej** ✅ | kierunek rzeczywisty | — | −180…+180° | wpisz deklinację dla miejscowości (`mag_decl` w *Kalibracja*) | `mot_heading` |

## 8. Czujniki Halla / kontaktrony / przełączniki

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **A3144 / AH3144 / SS49E / A1321** 🟡 | impulsy obrotowe (anemometr), licznik obrotów | `GPIO 6` / `GPIO 14` | 4,5…24 V (dzielnik!), 3,3 V | próg napięcia, filtr 100 ms | `wind`, licznik |
| **DRV5032 / DRV5033 (cyfrowe Hall)** 🟡 | bezstykowy przełącznik, pozycja | `GPIO 2` | 1,65…5,5 V | brak | zdarzenie w logu |
| **DRV5055 / DRV5056 (analogowe Hall)** 🟡 | pomiar liniowy pola (przesunięcie, poziom) | ADC1 | 0…3,3 V | offset i wzmocnienie liniowe | `pos` |
| **TMAG3001 / TMAG5170** 🟡 | Hall 3D (kąt dokładny) | I²C | ±300 mT | kalibracja kąta | `vane` |
| **Kontaktron / reed switch** 🟡 | zliczanie impulsów (deszcz, wiatr, obroty) | `GPIO 5` | 3,3 V, 1 mA | filtr drgań | `rain`, `wind` |
| **Krańcówka / przycisk na GPIO** ✅ | tryb AP, reset, ręczny wpis | `GPIO 0` | — | brak | zdarzenie w *Logi* |

## 9. Natężenie światła (lux)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **BH1750** ✅ | lux (nasłonecznienie) | I²C `0x23`/`0x5C` | 1…65 535 lx, ±20 % | współczynnik „lux na jednostkę” w *Kalibracja* | `light` |
| **VEML7700** ✅ | lux, 16-bit, wysoka rozdzielczość | I²C `0x10` | 0…120 klx | wsp. kalibracji (błąd kierunkowy) — kanał `light` tylko gdy brak BH1750 | `light` → kolumna `light` |
| **OPT3001 / OPT3002** 🟡 | lux zbliżony do oka ludzkiego | I²C `0x44`/`0x45` | 0,01…83 klx | j.w. | `light` |
| **LTR-329 / LTR-303** ✅ | lux + IR | I²C `0x29` | 0…64 klx | współczynnik + odjęcie IR | `light` |
| **LTR-390 / LTR-390UV** ✅ | UV (UVA/UVB/UV-index) — sterownik czyta kanał UVS | I²C `0x53` | 0…15 UVI | wsp. UV w *Kalibracja* (offset kanału `uv`) | `uv` → kolumna `uv` |
| **TSL2561 / TSL2591** ✅ | lux szerokopasmowy, wysoka czułość | I²C `0x29`/`0x39` | 0,01…88 klx | współczynnik i czas integracji | `light` |
| **ISL29125 / TCS34725 (RGB)** 🟡 | natężenie i barwa światła (podstawa pyranometru DIY) | I²C `0x44`/`0x29` | 16-bit na kanał | wzorzec: lampa kalibracyjna | `light`, `rgb` |

## 10. Promieniowanie UV

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **VEML6070** ✅ | UVA (jednokanałowy) | I²C `0x38`/`0x39` | 0…328 mW/m² | współczynnik UVI | `uv` |
| **VEML6075** ✅ | UVA + UVB, liczony UV-index | I²C `0x10` | 0…15 UVI | współczynniki A/B fabryczne + offset | `uv` |
| **LTR-390UV** ✅ | UVI z LTR-390UV (kanał UVS) | I²C `0x53` | 0…15 UVI | jak wyżej | `uv` |
| **SI1145 / SI1146 / SI1147** 🟡 | UV-index + IR + widzialne | I²C `0x60` | 0…11 UVI | współczynnik z krzywej fabrycznej | `uv`, `light` |
| **GUVA-S12SD / GY-ML8511** 🟡 | analogowy UV | ADC1 `GPIO 2` | 0…3,3 V | przeliczenie mV → UVI, 2 punkty | `uv` |
| **Zapisywanie UV do logów** ✅ (po dodaniu kanału) | kolumna `uv` w CSV + wykres | — | — | — | `uv` |

## 11. Promieniowanie słoneczne / pyranometr

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Pyranometr (W/m², np. z ogniwem kalibrowanym)** ✅ | energia słoneczna, nasłonecznienie | ADC1 lub wejście ADS1115 | 0…1200 W/m², ±5 % | czułość ogniwa `mV na W/m²` → `solar_mv_wm2` (NVS, *Kalibracja*) | `solar_wm2` → kolumna `solar_wm2` |
| **Panel słoneczny / ogniwo + rezystor bocznikowy** ✅ | tani pyranometr DIY | ADC1 (`PIN_PYRANO_ADC`) lub ADS1115 | 0…20 mV → W/m² | porównanie ze wzorcem w słoneczny dzień | `solar_wm2` |
| **MLX90614 skierowany w niebo** ✅ | temperatura nieba, zachmurzenie | I²C | −40…+125 °C | offset, emisyjność (`mlx_emiss` w *Kalibracja*) | `mot_ir_obj_t` |
| **Wskaźnik zachmurzenia (pochodna)** ✅ | 0…100 % pokrycia chmur | z `solar_wm2` i pozycji Słońca | — | normalizacja do wartości teoretycznej Słońca | karta *Zachmurzenie* w *Prognozie* |

## 12. Temperatura gleby

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **DS18B20 wodoodporny na głębokości 5/20/50 cm** ✅ | profil termiki gruntu, początek wegetacji | 1-Wire `GPIO 4` | −55…+125 °C | offset per sonda, izolacja kabla | `ds_*` |
| **NTC 10 k w sondzie glebowej** 🟡 | tania sonda | ADC1 | −40…+105 °C | tabela Steinharta–Harta | `temp` |
| **Sonda PT1000 (Modbus/analog)** ✅ | dokładna temperatura gleby | MAX31865 / RS485 | ±0,15 °C | 3-przewodowo | `temp` |
| **SHT31 ze filtrem + rurka** ✅ | T/RH gleby (wilgotność względna gleby) | I²C (przedłużacz 2 m) | ±0,3 °C | offset w miejscu instalacji | `temp`, `hum` |

## 13. Wilgotność gleby

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Rezystancyjny (YL-69, FC-28)** ✅ | wilgotność objętościowa (poglądowo) | ADC1 `GPIO 2` (`PIN_SOIL_ADC`) | 0…3,3 V | **należy** kalibrować 2 punktami: `soil_dry_v` / `soil_wet_v` (NVS, *Kalibracja*); korozja elektrod | `soil` → kolumna `soil` |
| **Pojemnościowy (v1.2, v2.0, SMT)** ✅ | wilgotność gleby bez korozji (zalecany) | ADC1 `GPIO 2` | 0…3,3 V → % VWC | jak wyżej; suszenie i ważenie próbki, 3 punkty | `soil` → kolumna `soil` |
| **Sonda SHT / DHT w glebie** 🟡 | T + „RH gleby” | I²C / 1-Wire | — | jak wyżej | `soil`, `temp` |
| **Czujnik tensjometryczny / ciśnieniowy** 🟡 | siła ssąca gleby | ADC1 | 0…−100 kPa | krzywa fabryczna | `soil` |
| **SDI-12 (np. Teros 10/11/12)** 🟡 | profesjonalna wilgotność i temperatura gleby | UART 1200 baud, `GPIO 18`, adres `0` | ±3 % VWC | kalibracja z próbkami gleby | `soil`, `temp` |
| **Modbus RTU / RS485 (np. NPK, JXCT)** 🟡 | wilgotność, temperatura, pH, przewodność | MAX485 `RO 16`, `DI 17`, `DE 15`, 9600 8N1 | ±5 %, adres 1…247 | rejestry kalibracyjne czujnika | `soil`, `ec`, `ph` |
| **Wilgotność gleby jako „plik + wykres”** ✅ | kolumna `soil` w CSV i wykres dobowy | — | — | — | `soil` |

## 14. Jakość powietrza — pyły (PM)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **PMS5003 / PMS7003 / PMS6003** ✅ | PM1,0 / PM2,5 / PM10 | UART2 9600, RX 18 / TX 17 | 0…1000 µg/m³, ±10 % | ko-lokacja ze wzorcem, współczynnik korekty wilgotności | `pm1`, `pm25`, `pm10` |
| **PMSA003 / PMSA003I (I²C)** 🟡 | j.w. bez UART | I²C `0x12` | j.w. | j.w. | `pm25` |
| **SDS011 / SDS018 / SDS021** 🟡 | PM2,5 / PM10 (Nova Fitness) | UART 9600 | 0…999 µg/m³ | j.w. + tryb uśpienia | `pm25` |
| **SPS30** ✅ | PM1 / 2,5 / 4 / 10 + masa i liczba cząstek, ±10 % | I²C `0x69` (autodetekcja z SEN5x) | 0…1000 µg/m³ | tryb czyszczenia wentylatora co 7 dni (`start_fan_cleaning`) | `in_pm1`, `in_pm25`, `in_pm4`, `in_pm10` |
| **SEN54 / SEN55** ✅ | PM + RH + T + VOC + NOx (Sensirion) | I²C `0x69` | PM ±10 % | kompensacja RH/T, 24 h pracy ciągłej | `in_pm*`, `sen_t`, `sen_h`, `sen_voc`, `sen_nox` |
| **SEN50 / SEN51 / SEN5x** ✅ | jak wyżej (bez VOC/NOx) | I²C `0x69` | j.w. | j.w. | `in_pm*` |
| **HPMA115 / OPC-N3 / SPS6x** 🟡 | pyły (Honeywell / Alphasense) | UART / SPI | 0…1000 µg/m³ | ko-lokacja | `pm25` |
| **Przekrój: PM1 / PM2,5 / PM10** ✅ | wykres + kolumny `pm1`, `pm25`, `pm10` | — | — | — | `pm*` |

## 15. Jakość powietrza — VOC / IAQ (gazy organiczne)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **BME680 / BME688** 🟡 | IAQ, VOC, T, RH, P | I²C `0x76` | 0…500 kΩ | 48 h wygrzewania, **baseline** zapisany w NVS | `iaq`, `tvoc` |
| **SGP30** ✅ | TVOC (ppb) + eCO₂ (ppm) | I²C `0x58` | TVOC 0…60 000 ppb | 12 h pracy + baseline (`getBaseline`/`setBaseline`) w NVS | `tvoc`, `eco2` |
| **SGP40 / SGP41** 🟡 | VOC-index, NOx-index (Sensirion) | I²C `0x59` | index 0…500 | kompensacja RH/T z BME280 | `voc_idx`, `nox_idx` |
| **ENS160 / ENS161 (ScioSense)** 🟡 | AQI + TVOC + eCO₂ + T/RH | I²C `0x52`/`0x53` | AQI 1…5 | 3 × 1 h tryb „burn-in”, kalibracja z komorą odniesienia | `tvoc`, `eco2`, `aqi` |
| **CCS811 / CCS801** 🟡 | TVOC + eCO₂ | I²C `0x5A`/`0x5B` | 0…1187 ppb | 48 h burn-in + baseline w NVS | `tvoc`, `eco2` |
| **IAQ z gazów MQ (patrz rozdział 17)** 🟡 | orientacyjna jakość | ADC1 + dzielnik | — | R0 w powietrzu odniesienia | `gas` |
| **Zapis do CSV** ✅ | kolumny `co2`, `eco2`, `tvoc` | — | — | — | `co2`, `eco2`, `tvoc` |

## 16. Jakość powietrza — CO₂

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **SCD30 / SCD40 / SCD41** ✅ | CO₂ NDIR ±(30 ppm + 3 %), T, RH | I²C `0x62` | 400…40 000 ppm | **ABC** (auto baseline) po 7 dniach w świeżym powietrzu lub kalibracja jednopunktowa 400 ppm | `co2` |
| **SenseAir S8 / S8 LP** 🟡 | CO₂ ±40 ppm | UART/Modbus 9600 | 400…2000 ppm | kalibracja jednopunktowa (przycisk / komenda) | `co2` |
| **Sunrise / Sunrise Mini** 🟡 | CO₂ NDIR ±30 ppm | UART (ABC), 9600 | 400…5000 ppm | jak SCD4x | `co2` |
| **MH-Z19B / C / D** 🟡 | CO₂ NDIR (UART lub PWM) | UART 9600 / PWM `GPIO 21` | 400…5000 ppm | kalibracja zerem (400 ppm) w komendzie `0x03/0x87` | `co2` |
| **SGP30 eCO₂** ✅ | CO₂ „szacowane” (nie NDIR!) | I²C `0x58` | 400…60 000 ppm | baseline; nie zastępuje NDIR | `eco2` |
| **CCS811 eCO₂** ✅ | j.w. | I²C | 400…8192 ppm | jak wyżej | `eco2` |
| **Alarm CO₂ (wentylacja)** ✅ | dzwonek / RGB / MQTT powyżej progu | — | próg np. 1000 ppm | próg w *Konfiguracja* | zdarzenie w logu |

## 17. Gazy (CO, NOx, CH₄, LPG, alkohol, wodór)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **MQ-7** 🟡 | tlenek węgla (CO) — czad | ADC1 + obciążenie | 20…2000 ppm | 24–48 h burn-in, potencjometr `RL`, R0 w powietrzu odniesienia | `gas_co` |
| **MQ-131** ✅ | ozon (O₃) | ADC1 | 10…1000 ppb | jak wyżej | `gas_o3` |
| **MQ-136** ✅ | siarkowodór (H₂S) | ADC1 | 1…200 ppm | j.w. | `gas_h2s` |
| **MQ-137** ✅ | amoniak (NH₃) | ADC1 | 1…100 ppm | j.w. | `gas_nh3` |
| **MQ-5** 🟡 | gaz ziemny / LPG | ADC1 | 200…10 000 ppm | j.w. | `gas_ch4` |
| **MQ-6** 🟡 | LPG / butan | ADC1 | 200…10 000 ppm | j.w. | `gas_lpg` |
| **MQ-2 / MQ-135 / MQ-9** ✅ | dym / jakość powietrza / CO+CH₄ | ADC1 | — | j.w. | `gas` |
| **MiCS-4514 / MiCS-6814** ✅ | CO, NO₂, NH₃ + redukujące | I²C (z ADC) | 1…1000 ppm | 3 tryby grzania, kalibracja w 2 gazach | `gas_*` |
| **Czujnik elektrochemiczny (np. Alphasense CO-A4, NO2-A1)** 🟡 | pomiar analityczny | ADC (przetwornik LMP91000) | ppm / ppb | wzorzec gazowy — kosztowne | `gas` |
| **Czujnik katalityczny / półprzewodnikowy (TGS2600, TGS2611)** 🟡 | trend zanieczyszczeń | ADC1 | — | jak MQ | `gas` |
| **Zapis gazów** 🟡 | kolumny `gas_*` w CSV + wykres | — | — | — | `gas_*` |

## 18. Temperatura bezdotykowa (IR)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **MLX90614 (A/B/C/F)** ✅ | punktowy IR: niebo, grunt, śnieg | I²C `0x5A` (+PWM) | −70…+380 °C, ±0,5 °C | emisyjność (0,95 dla wody, 0,98 grunt), offset | `mot_ir_obj_t` |
| **MLX90615** 🟡 | punktowy IR (medyczny) | I²C `0x5B` | −40…+115 °C, ±0,1 °C | j.w. | `temp_ir` |
| **MLX90632** 🟡 | SMD punktowy IR | I²C `0x3A` | −20…+200 °C | j.w. | `temp_ir` |
| **MLX90640 / MLX90641 (32×24)** 🟡 | matryca termowizyjna — chmury, izolacja domu | I²C `0x33` | −40…+300 °C | emisyjność + korekta tła, 4 Hz | `temp_ir`, mapa |
| **AMG8833 / Grid-EYE** 🟡 | 8×8 termowizja | I²C `0x69` | 0…80 °C, ±2,5 °C | offset w polu widzenia | `temp_ir` |
| **MLX90614 + niebo (zachmurzenie)** 🟡 | 100 % chmur = ΔT ≈ 0 | I²C | — | zależność empiryczna, kalibracja w bezchmurną noc | `cloud` |

## 19. Dodatkowe DS18B20 i pomiary wielopunktowe

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **DS18B20 ×8 (do 1-Wire)** ✅ | 8 punktów pomiarowych jednocześnie | `GPIO 4` + 4,7 kΩ | −55…+125 °C, ±0,5 °C | offset per indeks, adres ROM widoczny w *Czujniki* | `ds_0…ds_7` |
| **DS18S20 / DS1822** 🟡 | starsze odpowiedniki | 1-Wire | mniejsza rozdzielczość | j.w. | `ds_*` |
| **DS28EA00 / DS2431 (z adresacją)** 🟡 | łańcuch z identyfikacją pozycji | 1-Wire | j.w. | kolejność wg ROM | `ds_*` |
| **MAX31820** ✅ (kompatybilny z DS18B20) | j.w. w wersji TO-92 | 1-Wire | j.w. | j.w. | `ds_*` |

## 20. Poziom wody / odległość

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **VL53L0X** ✅ | odległość do 2 m (poziom wody, śnieg) | I²C `0x29` | 30 mm…2 m, ±3 % | offset zera (poziom odniesienia) | `mot_tof_dist` |
| **VL53L1X** ✅ | do 4 m, szybszy | I²C `0x29` | 40 mm…4 m | j.w. | `mot_tof_l1_dist` |
| **VL53L4CD** 🟡 | krótki zasięg, bardzo dokładny (poziom w studni) | I²C `0x29` | 1…130 cm | j.w. | `level` |
| **VL53L5CX / VL53L8CX** 🟡 | matryca 8×8 ToF (fala, śnieg, woda) | I²C `0x29` | 4 m, 15/60 Hz | j.w. + filtr medianowy | `level`, mapa |
| **HC-SR04 / HC-SR04P** 🟡 | ultradźwiękowy poziom (zbiornik) | `TRIG 21`, `ECHO 47` (5 V → dzielnik!) | 2 cm…4 m, ±3 mm | offset, kompensacja temperatury | `level` |
| **JSN-SR04T / A02YYUW (wodoodporny)** 🟡 | poziom w zbiorniku na zewnątrz | UART / TRIG-ECHO | 20 cm…6 m | j.w., ochrona przed szronem | `level` |
| **Czujnik pływakowy (kontaktron)** 🟡 | stan „woda powyżej progu” | `GPIO 2` | — | brak | zdarzenie |
| **CZujnik ciśnienia hydrostatycznego (4–20 mA)** 🟡 | poziom w zbiorniku/studni | ADS1115 (I²C `0x48`) + rezystor | 0…10 m | zerowanie + skala | `level` |
| **Pomiar poziomu śniegu (ultrasonic + T)** 🟡 | grubość pokrywy śnieżnej | jak HC-SR04 | 0…3 m | zerowanie na gruncie bez śniegu | `snow` |

## 21. Ultradźwięki — odległość i wykrywanie

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **HC-SR04 / HC-SR04+** 🟡 | odległość (poziom wody, śnieg) | TRIG/ECHO | 2 cm…4 m | kompensacja temperatury z `temp` | `level` |
| **JSN-SR04T (wodoodporny)** 🟡 | j.w. na zewnątrz | TRIG/ECHO lub UART | 20 cm…6 m | j.w. | `level` |
| **A02YYUW (UART, IP67)** 🟡 | j.w., wersja przemysłowa | UART 9600 | 3 cm…4,5 m | offset, filtr medianowy | `level` |
| **US-100 / US-016** 🟡 | tanie ultradźwięki | UART / analog | 2 cm…4,5 m | j.w. | `level` |
| **MB1000 / LV-MaxSonar** 🟡 | bezkontaktowy, analogowy odczyt | ADC1 | 0…6 m | krzywa liniowa | `level` |

## 22. IMU / akcelerometry / żyroskopy

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **MPU6050 / MPU6500 / MPU9250** ✅ | przechył, drgania masztu, detekcja ruchu | I²C `0x68`/`0x69` | ±2…16 g, ±250…2000 °/s | offset bias, kalibracja poziomicą | `mot_pitch`, `mot_roll`, `mot_vib` |
| **MPU6886 / ICM-20602** 🟡 | j.w. mniejszy | I²C `0x68` | j.w. | j.w. | `tilt` |
| **ICM-42688-P / ICM-20948** 🟡 | IMU 6/9-osiowy, dokładny | I²C/SPI `0x68` | j.w. | j.w. | `tilt`, `vane` |
| **BMI270 / BMI160 / BMI088** 🟡 | IMU niski pobór (Bosch) | I²C `0x68` | j.w. | j.w. | `tilt` |
| **LIS3DH / LIS2DW12 / LIS3LV02DL** 🟡 | akcelerometr (detekcja wstrząsu, drgań) | I²C `0x18`/`0x19` | ±2…16 g | j.w. | `mot_acc_x`, `mot_acc_y`, `mot_acc_z` |
| **ADXL345 / ADXL355 / ADXL362** 🟡 | akcelerometr (ADXL355 bardzo stabilny) | I²C `0x53`, SPI | ±2…16 g | j.w. | `mot_acc_x`, `mot_acc_y`, `mot_acc_z` |
| **BNO055 / BNO085 / BNO086** 🟡 | IMU z fuzją i kompasem (orientacja bezwzględna) | I²C `0x28`/`0x4B` | 9 DOF, ±2000 °/s | kalibracja 3D (ósemka), magnetometr patrz rozdz. 7 | `heading` |
| **QMI8658 / BNO08x (kombajn)** 🟡 | j.w. | I²C | j.w. | j.w. | `heading` |
| **Detekcja wstrząsu masztu** 🟡 | alarm wichury / oblodzenia | — | próg przyspieszenia | próg w kodzie | zdarzenie |

## 23. GPS / GNSS

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **NEO-6M** ✅ | pozycja + dokładny czas (RTC stacji) | UART1 9600, `RX 21`, `TX 47` | 2,5 m CEP, 1 Hz | brak (można ustawić SBAS) | `gps` |
| **NEO-M8N / M8Q** ✅ | dokładniejszy czas i pozycja | UART 9600/38400 | 2,0 m, 10 Hz | j.w. | `gps` |
| **NEO-M9N / M9V** ✅ | wielokonstelacyjny (Galileo/BeiDou) | UART 38400 | 1,5 m | j.w. | `gps` |
| **MAX-M10S / MAX-M8Q** 🟡 | mały, niski pobór | I²C `0x42` lub UART | 1,5 m | j.w. | `gps` |
| **ZED-F9P / ZED-F9R (RTK)** 🟡 | centymetrowa dokładność (korekcja NTRIP) | UART + USB | 1 cm + 1 ppm | konfiguracja korekcji NTRIP | `gps` |
| **Czas z GPS dla logów** 🟡 | dokładny timestamp w CSV i MQTT | — | ±1 µs | synchronizacja po fixie 3D | `datetime` w CSV |

## 24. Pozycja Słońca (obliczana)

| Pozycja | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Wysokość i azymut Słońca** 🟡 | kąt padania, przewidywanie nasłonecznienia, sterowanie roletą | z pozycji GPS + czasu | ±0,01° | poprawna strefa czasowa i DST | `sun_el`, `sun_az` |
| **Wschód / zachód / górowanie** ✅ | sterowanie oświetleniem i logami dobowymi | j.w. | ±1 min | j.w. | w *Pulpit* |
| **Współczynnik „clear sky”** 🟡 | zachmurzenie = 1 − solar/solar_max | z `solar` + pozycji Słońca | 0…1 | porównanie z pyranometrem | `cloud` |

## 25. Zasilanie i bateria

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **INA219** 🟡 | napięcie, prąd, moc | I²C `0x40` | 0…26 V, ±3,2 A | rezystor bocznikowy `R_shunt` | `vbus`, `current` |
| **INA226** 🟡 | j.w. dokładniejszy | I²C `0x40` | 0…36 V, ±20 A | `R_shunt`, `MaxCurrent` | `vbus`, `current` |
| **INA228 / INA238** 🟡 | 20-bit, energia | I²C `0x40` | ±85 V, dokładność 0,1 % | j.w. | `vbus`, `energy` |
| **INA3221** 🟡 | 3 kanały (panel, bateria, obciążenie) | I²C `0x40` | 0…26 V | j.w. | `vbus*` |
| **ACS712 / ACS758** 🟡 | prąd (Hall, izolowany) | ADC1 (dzielnik + 2,5 V ref) | ±5…50 A | offset przy zerowym prądzie, czułość mV/A | `current` |
| **MAX17048 / MAX17049 / MAX17055** 🟡 | % naładowania baterii Li-Ion | I²C `0x36` | ±1 % SOC | chemia ogniwa w kodzie | `soc` |
| **LC709203F** 🟡 | j.w. | I²C `0x0B` | ±1 % | typ ogniwa | `soc` |
| **Pomiar napięcia baterii (dzielnik)** ✅ | napięcie zasilania (alarm) | ADC1 `GPIO 3` | 0…20 V | współczynnik dzielnika | `vbat` |
| **Monitoring pojemności ogniwa Li-Po/Li-Ion + PV** 🟡 | bilans energetyczny stacji | jak wyżej | — | — | `vbat`, `current` |

## 26. Czujniki do ogrodu / rolnicze

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **Mokrość liścia (leaf wetness)** 🟡 | ryzyko chorób roślin | ADC1 `GPIO 14` | 0…100 % | sucho/pod wodą, 2 punkty | `leaf` |
| **Wilgotność gleby (rozdz. 13)** 🟡 | podlewanie | ADC1 / RS485 | % VWC | suszenie i ważenie | `soil` |
| **Temperatura gleby (rozdz. 12)** ✅ | wegetacja | 1-Wire / ADC | °C | offset | `ds_*` |
| **Natężenie światła PAR (S2-131)** 🟡 | fotosyntetycznie czynne promieniowanie | ADC1 | 0…2000 µmol/m²s | współczynnik fabryczny | `par` |
| **Anemometr + wiatrak na dachu (ochrona przed wiatrem)** 🟡 | j.w. | jak anemometr | — | j.w. | `wind` |
| **Czujnik oblodzenia (element rezystancyjny / wibracyjny)** 🟡 | alarm gołoledzi | ADC1 / I²C | — | tabela T-wilgotność | `ice` |
| **Czujnik opadów + licznik dzienny** ✅ | dobowe i miesięczne sumy | jak deszczomierz | mm | mm/impuls | `rain` |

## 27. Czujniki radarowe / obecności (opcjonalne)

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **HLK-LD2410 / LD2410B** 🟡 | obecność człowieka, „intruz” przy stacji | UART 256 000 | do 6 m | strefy i czułości w aplikacji | zdarzenie |
| **HLK-LD2450** 🟡 | pozycja i śledzenie w polu 100° | UART | do 8 m | j.w. | zdarzenie |
| **HLK-LD2461** 🟡 | praca w trybie tylko „obecność” | UART | do 8 m | j.w. | zdarzenie |
| **Dźwięk / natężenie ruchu (BCM2835 + mikrofon)** 🟡 | hałas otoczenia | ADC1 | dB(A) | wzorzec dźwięku | `noise` |

## 28. Wyświetlacze i interfejs lokalny (nie czujniki, ale moduły stacji)

| Moduł | Co daje | Podłączenie | Parametry | Uwagi | — |
|---|---|---|---|---|---|
| **Pierścień WS2812B / RGBIC 36 px** ✅ | kolor pogody, animacje, alarmy | `GPIO 7`, 5 V | 5 V, ~1 A max | jasność i progi w zakładce *Led RGB* | — |
| **OLED SSD1306 / SH1106 128×64** 🟡 | lokalny podgląd danych | I²C `0x3C` | 3,3 V | adres w konfiguracji | — |
| **LCD 1602 / 2004 (I²C PCF8574)** 🟡 | prosty wyświetlacz | I²C `0x27`/`0x3F` | 5 V | jasność przez PWM | — |
| **LCD TFT / e-ink (SPI)** 🟡 | ekran graficzny / wykres | SPI (`CS 15`) | 3,3 V | biblioteka np. TFT_eSPI | — |
| **Buzzer / przekaźnik alarmowy** 🟡 | sygnalizacja progów (wiatr, deszcz, CO₂) | `GPIO 21` | 3,3 V | logika i progi w *Konfiguracja* | — |

## 29. Karta microSD, logi i wykresy

| Element | Co daje | Podłączenie | Uwagi |
|---|---|---|---|
| **microSD 8 GB (SD_MMC 1-bit lub SPI)** ✅ | logi CSV, eksport, kopia NVS | SD_MMC: `CLK 40`, `CMD 41`, `D0 42`; SPI: `CS 10`, `SCK 12` | pliki w `/logs/RRRR-MM.csv`, obudowa FAT32 |
| **Zapis interwałowy** ✅ | próbka co `interwał` s (zakładka *Konfiguracja*) | — | kolumny rosną w miarę dodawania czujników |
| **Eksport plików** ✅ | pobranie CSV przez WWW (*Karta SD*) | — | eksport pojedynczego pliku lub ZIP-a |
| **Formatowanie karty** ✅ | inicjalizacja nowej karty | — | **usuwa wszystkie dane** — potwierdzenie w UI |
| **Wykresy dzień / tydzień / miesiąc / rok** ✅ | z plików CSV (`/api/logs`) | — | wybór wskaźnika z listy wykrytych czujników |
| **Kopia ustawień (NVS → plik)** ✅ | przenoszenie konfiguracji | — | `/config.json` na karcie |

**Kolejność kolumn w pliku CSV** (`/logs/RRRR-MM.csv`):

```
timestamp,datetime,temp,hum,press,light,ds0..ds7,rain,wind,vane,pm1,pm25,pm10,co2,eco2,tvoc,
sht_t,sht_h,bmp_t,bmp_p,uv,solar_wm2,soil,mmc_hdg,in_pm1,in_pm25,in_pm4,in_pm10,
sen_t,sen_h,sen_voc,sen_nox,ads0,ads1,ads2,ads3,
x2_temp,x2_hum,x2_press,x2_light,x2_rain,x2_wind,x2_vane,x2_pm1,x2_pm25,x2_pm10,
x2_co2,x2_eco2,x2_tvoc,x2_uv,x2_solar_wm2,x2_soil,x2_mmc_hdg,
x2_in_pm1,x2_in_pm25,x2_in_pm4,x2_in_pm10,x2_sen_t,x2_sen_h,x2_sen_voc,x2_sen_nox,
x2_sht_t,x2_sht_h,x2_bmp_t,x2_bmp_p,x2_ads0,x2_ads1,x2_ads2,x2_ads3,
x2_ds0..x2_ds7
```

Kolumny z prefiksem `x2_` to **te same wielkości zmierzone na węźle o adresie 2** (magistrala RS485,
patrz rozdział *Węzły po RS485* w [README.md](../README.md)). Lista kolumn zdalnych
odpowiada 1:1 kolumnom lokalnym, więc każdy czujnik podłączony do węzła jest zapisywany w CSV
i widoczny na wykresach. Kanały węzłów o **innych adresach** (`xN_<id>`) trafiają do dziennika
długiego `/logs/extra-RRRR-MM.csv` (wykresy czytają je stamtąd).

Kolumnę identyfikuje nagłówek — starsze pliki (bez ostatnich kolumn) nadal się otwierają,
a nowe metryki wystarczy dopisać na końcu listy, żeby nie przesunąć istniejących kolumn.

**Dziennik długi — czujniki bez własnej kolumny** (`/logs/extra-RRRR-MM.csv`):

```
timestamp,datetime,id,value
1758301234,2026-09-19 23:00:34,vl53l0x_dist,1284.00
1758301234,2026-09-19 23:00:34,mpu_a_x,0.02
```

Każdy kanał z nowego sterownika modułowego (`drv_*.cpp`), który nie ma jeszcze kolumny
w szerokim pliku, zapisuje się automatycznie w tym pliku (jedno pole na kanał w każdym
interwale zapisu). Dzięki temu nowy czujnik od razu działa na **wykresach**
(`/api/logs?metric=<id>` czyta ten plik, gdy nie ma kolumny) i w analizie w Excelu —
bez rozszerzania nagłówka szerokiego CSV i bez psucia starszych plików.

## 30. MQTT / Home Assistant i serwisy zewnętrzne

| Element | Co daje | Uwagi |
|---|---|---|
| **MQTT → HA (auto-discovery)** ✅ | encje temperatur, PM, CO₂, wiatru, kierunku (kompas), opadu | prefiks i nazwa stacji w *Konfiguracja* |
| **Kierunek wiatru jako kompas** ✅ | N / NE / E … w `vane_dir/state` | `mdi:compass` |
| **Publikacja raportu serwisów** ✅ | temat `<prefiks>/services/state` | JSON ze statusem każdego serwisu |
| **Wysyłka do serwisów zewnętrznych** ✅ | Weather Underground, PWSWeather, Windy, OpenWeather, ThingSpeak, własny URL | dane z czujników „na zewnątrz”, tylko aktywne i wykryte |
| **Tylko czujniki zewnętrzne** ✅ | przełącznik w *Serwisy* | chroni przed wysłaniem temperatury pokojowej |
| **Wybór kanału temperatury** ✅ | np. DS18B20 na zewnątrz zamiast BME280 | lista kanałów z konfiguracji |
| **Uwaga o TLS** ✅ | wysyłka HTTPS bez walidacji certyfikatu (`setInsecure`) | celowo: brak pamięci na łańcuch CA — opisane w README |

---

## 31. Rekomendowany zestaw stacji (rysunek)

```
                          ┌────────────────────── antena Wi-Fi IPEX ──────────────────────┐
                          │                                                              │
   ┌──────────────────────┴──────────────────────────────────────────────────────────────┴────┐
   │                       ESP32-S3 WROOM-1  N16R8  (16 MB flash + 8 MB PSRAM octal)          │
   └──┬───────────┬──────────────┬───────────────┬──────────────┬───────────────┬─────────────┘
      │ I²C       │ I²C          │ UART2         │ UART1        │ 1-Wire        │ GPIO / ADC
      │ 8 / 9     │ 8 / 9        │ 17 / 18       │ 21 / 47      │ 4             │ 1..3, 5, 6, 14
      │           │              │               │              │               │
  ┌───┴────┐ ┌────┴──────┐ ┌─────┴──────┐ ┌──────┴─────┐ ┌──────┴──────┐ ┌──────┴─────────────┐
  │ BME688 │ │ SCD41     │ │ SEN55      │ │ GPS NEO-M9N│ │ DS18B20 ×4  │ │ anemometr  GPIO 6  │
  │ T/RH/P │ │ CO₂ NDIR  │ │ PM1/2,5/10 │ │ (NMEA)     │ │ gleba/woda  │ │ wiatrowskaz GPIO 1 │
  │ + IAQ  │ │ + T + RH  │ │ VOC + NOx  │ │            │ │             │ │ deszczomierz GPIO 5│
  └────────┘ └───────────┘ └────────────┘ └────────────┘ └─────────────┘ └────────────────────┘
      │            │              │
      │            │              ├──── SPS30 (I²C 0x69)  – pyły 7 frakcji, dokładniejszy
      │            │              └──── MLX90640 (I²C)     – termowizja 32×24 (chmury, grunt)
      │            │
      │            └──── opcjonalnie: PT1000 + MAX31865 (SPI) – temperatura wzorcowa
      │
      ├──── BH1750 (I²C 0x23) – lux   ·   VEML6075 (0x10) – UV   ·   MMC5983MA (0x30) – kompas
      ├──── INA228 (0x40) – napięcie/prąd/energia   ·   MAX17048 (0x36) – stan baterii
      ├──── VL53L5CX (0x29) – poziom śniegu / wody  ·   mokrość liścia (GPIO 14)
      └──── SD 8 GB (SD_MMC 40/41/42)   ·   WS2812B 36 px (GPIO 7, 5 V)   ·   przycisk GPIO 0
```

> ⚠️ **BME688, SCD41 i SEN55 nie mogą być w szczelnej obudowie!**
> Potrzebują swobodnego przepływu powietrza (DIN, kanał wentylacyjny lub perforowany komin
> od dołu obudowy), inaczej wilgotność i CO₂ są zafałszowane. Czujniki umieszczamy
> najniżej w obudowie, elektronikę i zasilanie wyżej. Wiatromierz, anemometr i deszczomierz
> na maszcie 1,5–2 m nad dachem, w promieniu minimum 10 m od przeszkód.

## 32. Wejścia analogowe — ADS1115 (rozbudowa ADC)

Wszystkie piny ADC1 w ESP32-S3 są już zajęte (1 = wiatrowskaz, 2 = gleba, 4 = OneWire,
5/6 = deszcz/wiatr, 7 = LED RGB, 8/9 = I²C, 10 = CS karty SD w trybie SPI). Dlatego każdy
kolejny czujnik analogowy (pyranometr, tensjometr, 4–20 mA, ogniwo, sonda rezystancyjna)
podłączamy przez **ADS1115** na I²C.

| Czujnik | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **ADS1115** ✅ | 4 niezależne wejścia analogowe, 16 bit | I²C `0x48` (ADDR→VDD = `0x49`, →SDA = `0x4A`, →SCL = `0x4B`) | 0…3,3 V, ±6,144 V (PGA), offset 8 µV | przelicznik napięcie → wielkość fizyczna w *Kalibracja* | `ads_0`…`ads_3` → kolumny `ads0`…`ads3` |
| **Pyranometr / ogniwo na ADS1115** ✅ | nasłonecznienie w W/m² | `A0` + rezystor bocznikowy | 0…3,3 V | czułość `solar_mv_wm2` (mV na W/m²) | `solar_wm2` |
| **Czujnik hydrostatyczny 4–20 mA** 🟡 | poziom wody w zbiorniku | `A1` + rezystor 150 Ω | 0…10 m | zerowanie + skala | `ads_1` |
| **Sonda tensjometryczna / rezystancyjna** 🟡 | siła ssąca gleby, EC | `A2` | 0…3,3 V | krzywa fabryczna | `ads_2` |

**Podłączenie:** `VDD` → 3V3, `GND` → GND, `SDA`/`SCL` → GPIO 8/9, `A0…A3` → czujniki,
`ALERT` nieużywany. Wejścia **nie mogą** przekroczyć 3,3 V — dla czujników 5 V i pętli
4–20 mA stosujemy dzielnik napięcia lub rezystor pomiarowy.

> **Kanały `ads_0…ads_3` są domyślnie wyłączone** (zazwyczaj używa się jednego wejścia).
> Włączasz je w zakładce **Kalibracja** → sekcja kanałów, a napięcie pojawia się wtedy
> na wykresach, w CSV (`ads0…ads3`) i w MQTT.

---

## 33. I²S / PDM — mikrofon i audio

Magistrala I²S to **osobne linie**, nie I²C: `BCLK` (zegar), `WS`/`LRCK` (kanał) i dane —
`DIN` (mikrofon, dane do ESP) albo `DOUT` (głośnik/DAC, dane z ESP). W tej stacji
proponowane piny to **BCLK = GPIO 41, WS = GPIO 42, DIN = GPIO 40, DOUT = GPIO 45**
(wszystkie wolne przy domyślnym podłączeniu karty SD po SPI `10…13`; gdybyś przełączył
kartę na tryb SD_MMC, kolidują z `CLK/CMD/D0`).
Mikrofon pozwala wykrywać deszcz, gradobicie i poziom hałasu, a głośnik zapowiadać
alarmy (burza, przymrozek, wiatr).

| Czujnik / moduł | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **INMP441** ✅ | mikrofon cyfrowy I²S: hałas w dB, wykrywanie deszczu/gradobicia | I²S `i2s_bclk`, `i2s_ws`, `i2s_din` — dowolne wolne GPIO (edytor pinów), 3V3 | 24 bit, 60 Hz…15 kHz, 1,4 mA | poziom odniesienia dB (cicha noc) → korekta `snd_offset_db` w *Kalibracja* | `snd_level`, `snd_peak`, `snd_leq` |
| **ICS-43434 / ICS-43432** 🟡 | mikrofon I²S (mały, wbudowany filtr) | I²S jak wyżej (obsługiwany jest ICS-43434) | 24 bit, SNR 65 dB | jak INMP441 | `snd_level` |
| **SPH0645LM4H** 🟡 | mikrofon I²S (Adafruit 3421) | I²S j.w., zasilanie 3V3 | 18 bit, −26 dBFS | offset + okno uśredniania 1 s | `noise` |
| **MSM261S4030H0 / MP34DT01 (PDM)** 🟡 | mikrofon PDM (jeden przewód danych + zegar) | PDM: `DATA 40`, `CLK 41`, 3V3 | SNR 61 dB | filtr decymacyjny, offset | `noise` |
| **MAX9814 / MAX4466 / KY-038 (analogowy)** 🟡 | tani mikrofon analogowy (detekcja deszczu) | przez ADS1115 `A3` (albo ADC1 `GPIO 2`) | 0…3,3 V, wzmocnienie 40/50/60 dB | próg napięcia dla „pada" | `rain` (binarnie) / `noise` |
| **MAX98357A** 🟡 | wzmacniacz + głośnik 3 W z I²S (zapowiedzi alarmów) | I²S `i2s_dout` (+ `i2s_bclk`), 5V | 3 W / 4 Ω | poziom w kodzie | — (wyjście TX w trybie ciszy, odtwarzanie jeszcze nie) |
| **PCM5102 / UDA1334A (DAC I²S)** 🟡 | wyjście liniowe audio (sygnały, dzwonek) | I²S j.w. + `VIN` 5V | 24 bit / 96 kHz | poziom w kodzie | — (wyjście) |

> **Uwaga:** sterownik I²S (`src/drv_i2s.cpp`) obsługuje **INMP441** i **ICS-43434**
> (mikrofon: `snd_level`, `snd_peak`, `snd_leq`) oraz instaluje wyjście TX dla
> **MAX98357A** (w trybie ciszy — odtwarzanie dźwięku nie jest jeszcze zaimplementowane).
> Role pinów `i2s_bclk`, `i2s_ws`, `i2s_din`, `i2s_dout` są w edytorze pinów; gdy
> którakolwiek z nich wynosi −1, moduł nie jest instalowany i nie publikuje danych.
> Korekta poziomu to `snd_offset_db`, okno uśredniania LEQ to `snd_leq_s`.
> Mikrofony **PDM** (MSM261S4030H0, MP34DT01), 18-bitowy SPH0645LM4H i mikrofony
> analogowe (MAX9814/MAX4466/KY-038) **nie** są jeszcze obsługiwane.

---

## 34. Pozostałe magistrale, ekspandery i moduły wykonawcze

Poza czujnikami stacja może obsłużyć moduły rozszerzające (dodatkowe piny, wyświetlacze,
przekaźniki, wagi, urządzenia Modbus). Pozycje 🟡 wymagają dopisania sterownika —
magistrala i piny są jednak ustalone, więc kabel można przygotować już teraz.

| Moduł | Co daje | Podłączenie | Parametry | Kalibracja | Wykres / log |
|---|---|---|---|---|---|
| **HX711** 🟡 | waga: deszczomierz wagowy, poziom zbiornika, śnieg | `DT 40`, `SCK 41`, 5V | 24 bit, 10/80 Hz | tara przy zerze + współczynnik g/zliczenie | `rain_w` / `tank` |
| **MAX31865** ✅ | PT100 / PT1000 (temperatura wzorcowa) | SPI bit-bang: `CS` = `rtd_cs` (dowolny GPIO), `SCK`/`MOSI`/`MISO` w edytorze pinów | −200…+600 °C, ±0,1 °C | rezystor odniesienia `Rref` (`rtd_rref`), `rtd_r0`, liczba przewodów (`rtd_wires`), filtr (`rtd_filter`) | `rtd_temp`, `rtd_res` |
| **MAX6675 / MAX31855** ✅ | termopary K/J/T (komin, piec) | SPI bit-bang: `CS` = `tc_cs`, `SCK`/`MISO` w edytorze pinów | −200…+1350 °C | typ termopary (`tc_type`), kompensacja zimnych końców wbudowana w MAX31855 | `tc_max6675`, `tc_31855_t` |
| **MCP3008 (10 bit) / MCP3208 (12 bit)** ✅ | 8 dodatkowych wejść analogowych (NTC, foto-dioda, miernik napięcia, elektroda pH) | SPI bit-bang: `CS` = `adc_cs` (**własna linia**), `SCK`/`MOSI`/`MISO` = `tc_*` w edytorze pinów | 0…Vref (domyślnie 3,3 V), 10/12 bit | napięcie odniesienia `adc_spi_vref` (0,5…5,5 V), typ układu wykrywany automatycznie | `adc_mcp_ch0` … `adc_mcp_ch7` |
| **MCP23017** ✅ | ekspander 16 dodatkowych GPIO na I²C | I²C `0x20…0x27` | 16 linii, przerwanie INT | — | — |
| **PCF8574 / PCF8575** ✅ | ekspander 8/16 GPIO (LCD, przekaźniki) | I²C `0x20…0x27` | 8/16 linii | — | — |
| **DS2482-100** 🟡 | mostek 1-Wire (długie kable, wiele DS18B20) | I²C `0x18` | do 8 magistral | jak DS18B20 | `ds_*` |
| **INA219 / INA226 / INA260** 🟡 | napięcie, prąd i moc (zasilanie, panel PV, grzałka) | I²C `0x40`/`0x41`/`0x44` | do 36 V, ±0,5 % | `R_shunt` + zero | `vbat`, `ibat`, `pbat` |
| **VL53L0X / VL53L1X** ✅ | pomiar odległości ToF: poziom wody, śniegu | I²C `0x29` | 30…2000 mm / 4 m, ±3 % | zerowanie przy znanym poziomie (`tof_offset_mm` w *Kalibracja*) | `mot_tof_dist`, `mot_tof_l1_dist` |
| **JSN-SR04T / HC-SR04** 🟡 | ultradźwięki: poziom, pokrywa śnieżna | `TRIG 40`, `ECHO 41` (dzielnik 1 kΩ/2 kΩ!) | 20…600 cm | zerowanie + korekta temperatury | `level` |
| **RC522 / PN532** 🟡 | RFID/NFC — dostęp serwisowy do ustawień | SPI (`CS 42`) lub I²C `0x24` | 13,56 MHz | — | — |
| **OLED SSD1306 (0x3C) / LCD 20×4 (0x27)** 🟡 | lokalny wyświetlacz na obudowie | I²C `0x3C`/`0x27` | 128×64 / 20×4 | — | — |
| **TFT ST7789 / ILI9341** 🟡 | ekran graficzny (wykresy lokalnie) | SPI: `CS 42`, `DC 40`, `RST 41` | 240×320 | — | — |
| **Urządzenia Modbus RTU** 🟡 | liczniki energii, ultradźwiękowy wiatr, rejestratory | RS485 `A/B` (`TX 38`, `RX 39`, `DE 45`) | 9600…115200 Bd, 8N1 | rejestry i skala w konfiguracji | zależnie od urządzenia |
| **Przekaźnik / SSR** 🟡 | sterowanie: nawadnianie, wentylator, grzałka | wolny GPIO (np. `45`) przez transoptor | 3,3 V / 10 mA | — | — (wyjście) |
| **Buzzer / syrena** 🟡 | alarm akustyczny (burza, przymrozek) | wolny GPIO (np. `45`) | 3,3–5 V | próg alarmu w kodzie | — (wyjście) |
| **Servo / PWM (SG90)** 🟡 | osłona radiacyjna, żaluzja, klapka | wolny GPIO (np. `42`) | 50 Hz PWM | zakres kąta w kodzie | — (wyjście) |
| **Kamera OV2640 / OV5640** 🟡 | zdjęcia na kartę SD i podgląd na stronie | SPI + DVP (piny do ustalenia), 3,3 V | 2 MP / 5 MP | ostrość i ekspozycja w kodzie | pliki JPG na SD |

> **Wolne piny na N16R8** (po domyślnym przypisaniu): `40`, `41`, `42` oraz `19`/`20`
> (USB — z ostrzeżeniem) i `3` (strapping). `47`/`48` zajmuje druga magistrala I²C dla RTC
> (jeśli włączona), `45` to `DE` RS485, a GPIO `26…37` są zajęte przez flash/PSRAM.
> Wszystkie przypisania zmieniasz w zakładce *Piny*, a podglądasz w legendzie poniżej tabel.

---

## Kalibracja — jak to robić w tej stacji

Wszystkie korekty wpisujesz w zakładce **Kalibracja**: `offset` (dodawany do pomiaru) oraz
`skala` (mnożnik). Wartości są zapisywane w NVS i stosowane do WWW, MQTT i serwisów zewnętrznych.

| Czujnik | Metoda | Praktyczny wzorzec |
|---|---|---|
| Temperatura (BME, SHT, DS18B20) | jednopunktowo: offset = T_wzorzec − T_stacja | termometr laboratoryjny, woda z lodem (0,0 °C) |
| Wilgotność | komora solna albo wilgotnościomierz referencyjny | NaCl 75 % RH, MgCl₂ 33 % RH |
| Ciśnienie | porównanie z pobliską stacją meteo **na poziomie morza** | podaj wysokość n.p.m. w *Konfiguracja* |
| Deszczomierz | 1 cykl = 0,2794 mm (albo wpisz zmierzoną wartość) | 100 ml wody przez lejek = policz tyki |
| Anemometr | współczynnik `km/h na Hz` (domyślnie 2,4) | przejazd samochodem lub porównanie z meteo |
| Kierunek wiatru | tabela 8 kierunków → napięcia ADC | ustaw wiatrowskaz na N i zapisz napięcie |
| Pyłomierz PM | współczynnik korekty wilgotności, ko-lokacja | pylometr referencyjny lub stacja miejska |
| CO₂ (SCD4x) | ABC po 7 dniach pracy ciągłej lub kalibracja do 400 ppm | wystaw na 20 min na świeże powietrze |
| VOC (SGP30/CCS811/BME680) | zapis baseline po 12–48 h pracy (NVS) | pomieszczenie wywietrzone |
| MQ-x | wypalanie 24–48 h, potem R0 w powietrzu odniesienia | pomiar `Rs/R0` |
| MLX90614 / MLX90640 | emisyjność + offset na wzorcu | ciało o znanej T, np. woda 20 °C |
| Poziom wody / śniegu (ToF, US) | zerowanie przy znanym poziomie | linijka + pomiar rzeczywisty |
| Gleba | 2 punkty napięcia: `soil_dry_v` (sucho) i `soil_wet_v` (pod wodą) | zanurz sondę w wodzie, potem w suchej glebie i zapisz oba napięcia |
| Nasłonecznienie / pyranometr | czułość ogniwa `solar_mv_wm2` (mV na W/m²) | porównanie z pyranometrem wzorcowym w słoneczny dzień (ok. 1000 W/m²) |
| Azymut (kompas MMC5983) | deklinacja magnetyczna `mag_decl` (°) dla Twojej lokalizacji | mapa deklinacji (np. +4,5° dla Polski) |
| Napięcie/prąd (INA2xx) | `R_shunt` + kalibracja offsetu zera | multimetr |

Sekcja **Kalibracja dodatkowych czujników** (zakładka *Kalibracja*) zapisuje powyższe wartości
w NVS pod kluczami `soil_dry_v`, `soil_wet_v`, `mag_decl`, `solar_mv_wm2`
(`POST /api/calibrate` z obiektem `{"extra":{…}}`), a podglądasz je w `GET /api/config`.

**Wskazówka:** po każdej kalibracji zapisz konfigurację do pliku (Karta SD → kopia zapasowa),
żeby można było szybko przywrócić ustawienia po aktualizacji OTA.

## Wykresy i logi dla nowych czujników — jak dodać kanał

Kanały „wbudowane” (BME280, DS18B20, deszcz, wiatr, PMS, SCD4x/SGP30) rejestruje
`src/sensors.cpp`. Kanały czujników dodatkowych z I²C/Analog wchodzą przez trzy haki
w `src/sensors_extra.cpp`, wołane z `SensorManager`:

| Hak | Kiedy się wykonuje | Co robi |
|---|---|---|
| `registerExtraChannels()` | w `begin()`, **przed** `applyChannelConfig()` | dopisuje kanały (`id`, `name`, `unit`, `decimals`: 0–3, `zone`, jednostka HA) |
| `beginExtra()` | w `begin()`, po starcie magistrali I²C | sonduje sprzęt (`ok()`), ustawia `setPresent(id, true)` |
| `readExtra()` | na końcu `readAll()` | czyta wartości i publikuje je przez `publishExtra()` |

1. W `src/sensors_extra.cpp` dodaj kanał w `registerExtraChannels()` i sterownik
   (np. w `src/drv_i2c.cpp`); wykrywanie sprzętu dopisz w `beginExtra()`.
2. Odczytaj wartość w `readExtra()`. Publikuj przez `publishExtra(id, v + offset)` —
   **tylko gdy `!isnan(v)`** (sam `NAN` fałszywie oznaczyłby czujnik jako obecny),
   a gdy pomiar ma zniknąć, użyj `clearExtra(id)`.
3. W `src/logger.cpp` dopisz `id` **na końcu** listy `COLS` oraz obsługę w `columnForMetric()`.
4. W `data/index.html` dodaj wpis w `metricName()` i `metricUnit()`; nazwę kanału dopisz
   do `data/i18n.js` (PL → EN/DE) i sprawdź `node tools/analyze_i18n.js`.
5. Jeśli kanał ma trafić do MQTT/HA, dopisz encję w `src/mqtt_client.cpp`;
   jeśli do serwisów zewnętrznych — w `src/weather_services.cpp` (`collect()` i `build*()`).
6. Kalibrację (offset/skala) ustawiasz w *Kalibracja* → `ConfigManager::channelCfg(id)`;
   wartości dodatkowe (np. `soil_dry_v`) trzymaj w `config.extraF()` / `setExtraF()`.

Dzięki temu każdy nowy czujnik automatycznie pojawia się w:
* zakładce **Pulpit** (karta, strefa wewnątrz/na zewnątrz),
* zakładce **Wykresy** (listwa wskaźników z `/api/sensors`),
* pliku CSV i w eksporcie,
* Home Assistant przez MQTT,
* serwisach zewnętrznych (jeśli jest to dana obserwacyjna).

## Bezpieczeństwo i zasilanie — zasady

* Wszystkie linie 5 V (PMS, SPS30, LED, przekaźniki) przez wspólną masę i kondensator 470–1000 µF.
* Wejścia ADC tylko 0…3,3 V — **dzielnik napięcia obowiązkowy** dla czujników 5 V i 4–20 mA.
* Linie 1-Wire dłuższe niż 3 m: skrętka + rezystor 4,7 kΩ + ewentualnie DS2482.
* Zasilanie stacji: 5 V / 2 A (LED 36 px przy pełnej jasności to ok. 1 A) + 470 µF przy płytce.
* Obudowa: elektronika w części suchej, czujniki powietrza w części z przepływem,
  przepusty kablowe od dołu (deszcz nie wlatuje).
* Wszystkie linie zewnętrzne warto zabezpieczyć transilem/BAT54S i diodą TVS przy płytce —
  burze i indukcja na długich kablach to najczęstsza przyczyna awarii.
