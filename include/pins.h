/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
// =============================================================
//  Mapa pinów - ESP32-S3 WROOM-1 N16R8
//  Tutaj dopasujesz piny do swojej płytki po zlutowaniu.
//
//  UWAGA na N16R8 (flash QIO + PSRAM octal):
//    - GPIO 26..37 są zajęte przez flash/PSRAM - NIE UŻYWAĆ
//    - GPIO 0 (boot), 3 (strapping), 19/20 (USB),
//      43/44 (UART0), 45/46 (strapping) - zostaw w spokoju
// =============================================================

// --- I2C: BME280 / BH1750 ---
#define PIN_SDA             8
#define PIN_SCL             9

// --- Zegar czasu rzeczywistego RTC (DS3231 / DS1307 / PCF8563) ---
//  Domyślnie RTC siedzi na WSPÓLNEJ magistrali I2C z czujnikami (PIN_SDA/PIN_SCL)
//  - wtedy nic nie trzeba zmieniać, wystarczy podłączyć moduł pod te piny.
//  Piny poniżej dotyczą tylko OSOBNEJ magistrali (I2C2 / Wire1), wybieranej
//  w zakładce "Zegar RTC" (magistrala = osobna). UWAGA: GPIO 48 to wbudowana
//  dioda RGB WS2812 na ESP32-S3-DevKitC-1 - firmware trzyma ten pin nisko,
//  żeby dioda nie świeciła. Do osobnego RTC wybierz inny wolny pin (np. 47/21
//  albo 47/20), bo 48 zostanie nadpisane jako wyjście LOW.
#define PIN_RTC_SDA2        47
#define PIN_RTC_SCL2        48

// --- OneWire: czujniki zewnętrzne DS18B20 (można kilka równolegle) ---
//  Domyślnie wyłączone (-1): GPIO 4 przejął analogowy czujnik UV (GUVA-S12SD).
//  DS18B20 podepniesz na wolnym pinie i ustawisz go w zakładce "Piny".
#define PIN_ONEWIRE         -1

// --- Deszczomierz (kontaktron, zwarty do GND przy impulsie) ---
#define PIN_RAIN            5

// --- Anemometr (kontaktron / hallotron) ---
#define PIN_ANEM            6

// --- Wiatrowskaz (dzielnik rezystorowy -> wejście analogowe) ---
#define PIN_VANE            1

// --- PMS5003 (UART2) - przyszły czujnik jakości powietrza.
//     Ustaw -1, aby całkowicie wyłączyć. PMS TX -> PIN_PMS_RX
#define PIN_PMS_RX          18
#define PIN_PMS_TX          17

// --- Karta microSD w trybie SPI (podstawowy sposób podłączenia).
//     Kolejność musi się zgadzać z fizycznym połączeniem modułu:
//       SCK -> GPIO 10, MOSI -> GPIO 11, CS -> GPIO 12, MISO -> GPIO 13.
//     Firmware przekazuje TE piny do SD.begin(), więc zmiana tutaj (albo
//     na stronie www w zakładce "Piny") w pełni wystarcza.
#define PIN_SD_SCK          10
#define PIN_SD_MOSI         11
#define PIN_SD_CS           12
#define PIN_SD_MISO         13

// --- Karta microSD: SDMMC 1-bit (opcja: mniej pinów, większa szybkość).
//     Domyślnie WYŁĄCZONE (-1), bo stacja czyta kartę po SPI. Jeśli
//     podłączysz kartę pod dedykowane piny SDMMC, wpisz tu numery
//     (np. 40/41/42) - firmware sam wybierze ten interfejs.
#define PIN_SDMMC_CLK       -1
#define PIN_SDMMC_CMD       -1
#define PIN_SDMMC_D0        -1

// --- LED statusu i przycisk (GPIO0 = BOOT, reset do fabrycznych) ---
//     Domyślnie wyłączony (-1): dioda statusu na płytce zbędnie pobiera
//     prąd, a jedynym wskaźnikiem ma być pierścień LED RGB. Chcesz migającą
//     diodę diagnostyczną? Wpisz tu numer GPIO albo włącz ją na stronie www
//     w zakładce "Piny" (pole "LED statusu").
#define PIN_LED             -1
#define PIN_BUTTON          0

// --- Pierścień LED RGB (WS2812B / RGBIC, 36 diod, 48 mm) ---
//     Zasilanie: 5V i GND z płytki, jeden przewód danych na GPIO.
//     Kolejność: DIN płytka -> DOUT pierścienia.
//     Rekomendacja: rezystor 330 om w linii danych i kondensator
//     1000 uF między 5V i GND przy pierścieniu.
#define PIN_RGB             7

// =============================================================
//  Magistrala RS485 - drugi ESP (węzeł z zewnętrznymi czujnikami)
//
//  Ta stacja jest MASTEREM i odpytywuje węzeł po bezpośredniej linii TTL
//  (3 przewody: master TX 38 -> RX węzła 20, master RX 39 -> TX węzła 21,
//  wspólny GND - bez konwertera MAX485). Przy dłuższym kablu ekran do GND
//  z jednej strony; skrętka i przeplot z GND ograniczają zakłócenia.
//  Piny są wolne na N16R8 i nie kolidują z niczym innym.
//  UART0 jest wolny, bo konsola idzie przez natywne USB
//  (ARDUINO_USB_CDC_ON_BOOT=1), UART1 zajmuje GPS, UART2 - PMS5003.
//  DE (-1 = moduł z automatycznym kierunkiem) przełącza nadawanie/odbiór;
//  przy tej 3-przewodowej linii TTL jest niepotrzebny (na płytce stacji
//  ustawione 47, węzeł C3 ma 10 - firmware trzyma je w stanie niskim).
//
//  Kolejność RX/TX jest celowo odwrotna (RX = 39, TX = 38): węzły na
//  ESP32-C3 SuperMini mają UART0 na GPIO 20/21, więc przy takim układzie
//  przewody idą "po kolei", bez krzyżowania:
//    master 38 (TX) -> RX węzła (20),  master 39 (RX) -> TX węzła (21).
//  Zasada nadrzędna: RX jednego ESP zawsze na TX drugiego (dwa ESP32-S3
//  trzeba więc skrzyżować - oba mają ten sam domyślny układ pinów).
// =============================================================
#define PIN_RS485_RX        39
#define PIN_RS485_TX        38
#define PIN_RS485_DE        45

// =============================================================
//  Przyszła kamera (OV2640/OV5640) - piny do ustalenia po
//  doborze modułu. Wtedy kartę SD przełącz na SPI (powyżej).
// =============================================================

// --- GPS (odbiornik NMEA 0183 na UART1) ---
//     GPS TX -> PIN_GPS_RX, GPS RX <- PIN_GPS_TX.
//     Odbiornik zasilany z 3V3 (NEO-6M ma własny stabilizator - wtedy 5V).
#define PIN_GPS_RX          15
#define PIN_GPS_TX          16

// --- Detektor wyładowań AS3935 (I2C 0x03 + linia przerwania) ---
#define PIN_AS3935_IRQ      14

// --- Czujniki analogowe (ADC) ---
//     ADC1 (GPIO 1-10) działa równolegle z Wi-Fi, ADC2 (GPIO 11-20) NIE.
//     Wszystkie piny ADC1 są już zajęte (1 = wiatrowskaz, 4 = UV analogowy,
//     5/6 = deszcz/wiatr, 7 = RGB, 8/9 = I2C, 10 = SCK karty SD w trybie SPI).
//     Dlatego domyślnie wolne zostaje tylko GPIO 2 - więcej czujników
//     analogowych podłącz przez ADS1115 na I2C (4 kanały, 16 bitów).
#define PIN_SOIL_ADC        -1    // sonda wilgotności gleby (GPIO2 przejął czujnik deszczu/śniegu)
#define PIN_PYRANO_ADC      -1    // pyranometr / ogniwo słoneczne (W/m2)
#define PIN_UV_ADC          4     // analogowy czujnik UV (GUVA-S12SD, 100 mV/UVI)
#define PIN_SCATTER_ADC     -1    // widzialność (rozproszenie światła)
#define PIN_GAS_ADC         -1    // MQ-2/5/7/135, MiCS-4514 (przez dzielnik)
#define PIN_GAS_ADC2        -1    // drugi czujnik gazów równolegle (np. MQ-7 + MQ-135)
#define PIN_PAR_ADC         -1    // czujnik PAR (S2-131, fotodioda) - W/m2 w zakresie 400-700 nm
#define PIN_LEAF_ADC        2     // mokrość liścia / deszcz-śnieg (FC-37/YL-83, wyjście A0)
#define PIN_ACS_ADC         -1    // ACS712 / ACS758 (pomiar prądu)

// --- Czujnik radarowy obecności HLK-LD2410 / LD2450 (UART 256000) ---
//     LINIA TX radaru -> RX ESP32 (odbiera raporty celu), opcjonalnie
//     LINIA RX radaru <- TX ESP32 (zmiana ustawień modułu).
#define PIN_RADAR_RX        -1
#define PIN_RADAR_TX        -1

// =============================================================
//  Czujniki cyfrowe i magistrale opcjonalne
//  (domyślnie -1 = wyłączone; włącz w zakładce "Piny" po zlutowaniu,
//   wybierając wolny pin na swojej płytce)
// =============================================================

// --- DHT11 / DHT22 / AM2302 (jeden przewód danych + 4,7 kΩ do 3V3) ---
//     GPIO 42 = wolny pin złącza J3 (grupa JTAG 39-42, czyli bez zewnętrznego
//     debuggera); 40/41 zostają wolne. Tryb SDMMC 1-bit (40/41/42) koliduje.
#define PIN_DHT             42

// --- Czujnik Tuya temp./wilg. (TLSR8258, firmware UART) ---
//     TX czujnika -> RX ESP32. Linia ASCII "T=xx.xx;RH=yy.yy" co ~2 s,
//     115200 8N1. Odczyt bit-bang (SoftwareSerial), bo wszystkie 3 sprzętowe
//     UART-y są zajęte (RS485 / GPS / PMS5003). GPIO 41 = wolny pin grupy JTAG.
#define PIN_TUY_RX          41

// --- Waga / tensjometr / siła na HX711 (2 przewody: DT i SCK) ---
#define PIN_HX711_DT        -1
#define PIN_HX711_SCK       -1

// --- Odległość ultradźwiękowa: HC-SR04, JSN-SR04T, A02YYUW (TRIG/ECHO) ---
#define PIN_US_TRIG         -1
#define PIN_US_ECHO         -1

// --- Termopary i RTD na SPI: MAX6675, MAX31855, MAX31865 (PT100/PT1000) ---
//     Wszystkie trzy układy dzielą jedną magistralę SCK/MOSI/MISO,
//     różnią się tylko linią CS - dlatego tc_cs i rtd_cs są osobne.
#define PIN_TC_SCK          -1
#define PIN_TC_MOSI         -1
#define PIN_TC_MISO         -1
#define PIN_TC_CS           -1    // MAX6675 / MAX31855
#define PIN_RTD_CS          -1    // MAX31865
#define PIN_ADC_CS          -1    // MCP3008 / MCP3208 (osobny CS, wspólne SCK/MOSI/MISO z tc_*)

// --- Audio / mikrofon I²S: INMP441, ICS-43434, SPH0645, MAX98357A ---
//     din = dane z mikrofonu (SD), dout = dane do wzmacniacza (DIN).
#define PIN_I2S_BCLK        -1
#define PIN_I2S_WS          -1
#define PIN_I2S_DIN         -1
#define PIN_I2S_DOUT        -1

// --- Ekspandery portów: MCP23017, PCF8574/8575 (linia przerwania INT) ---
#define PIN_EXP_INT         -1

