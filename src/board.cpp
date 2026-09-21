/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "board.h"
#include "pins.h"
#include "pinmap.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

// ------------------------------------------------------------
//  Na jakim chipie zbudowano firmware?
// ------------------------------------------------------------
enum ChipKind { CHIP_S3 = 0, CHIP_C3, CHIP_C6 };

#if defined(CONFIG_IDF_TARGET_ESP32C3)
static constexpr ChipKind kChip = CHIP_C3;
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
static constexpr ChipKind kChip = CHIP_C6;
#else
static constexpr ChipKind kChip = CHIP_S3;
#endif

// ------------------------------------------------------------
//  Szablony pinów dla płytek innych niż ESP32-S3 (N16R8).
//  Pełne zestawy (wszystkie funkcje), żeby domyślne piny z pins.h
//  nigdy nie "przeciekły" na płytkę o innym układzie GPIO.
// ------------------------------------------------------------

// ESP32-C3 SuperMini: GPIO 0-10 i 18-21 (11-17 flash, 18/19 USB,
// 20/21 UART0 - u nas RS485, 2/8/9 strapping).
//
// Węzeł "wiatr" (typowy pierwszy moduł na zewnątrz):
//   - AS5600 (wiatrowskaz magnetyczny) na I2C: SDA = GPIO 8, SCL = GPIO 9,
//   - kontaktron / Hall anemometru: GPIO 5,
//   - wiatrowskaz analogowy (potencjometr) wyłączony - kierunek daje AS5600,
//   - deszczomierz wyłączony: siedzi na OSOBNEJ płytce (kolejny węzeł).
// Diody statusu i przycisku BOOT nie ma, bo GPIO 8 (LED) i GPIO 9 (BOOT)
// przejmuje magistrala I2C - jedna funkcja na pin.
static const BoardPinDef C3_PINS[] = {
  {"i2c_sda",      8},
  {"i2c_scl",      9},
  {"onewire",      4},
  {"rain",         -1},
  {"anem",         5},
  {"vane",         -1},
  {"pms_rx",       -1},
  {"pms_tx",       -1},
  {"sdmmc_clk",    -1},
  {"sdmmc_cmd",    -1},
  {"sdmmc_d0",     -1},
  {"sd_cs",        -1},
  {"sd_sck",       -1},
  {"sd_mosi",      -1},
  {"sd_miso",      -1},
  {"led",          -1},
  {"button",       -1},
  {"rgb",          2},
  {"gps_rx",       -1},
  {"gps_tx",       -1},
  {"rs485_rx",     20},
  {"rs485_tx",     21},
  {"rs485_de",     10},
  {"as3935_irq",   -1},
  {"soil_adc",     1},
  {"pyrano_adc",   -1},
  {"scatter_adc",  -1},
  {"gas_adc",      -1},
  {"gas_adc2",     -1},
  {"par_adc",      -1},
  {"leaf_adc",     -1},
  {"acs_adc",      -1},
  {"radar_rx",     -1},
  {"radar_tx",     -1},
  {"dht",          -1},
  {"us_trig",      -1},
  {"us_echo",      -1},
  {"hx711_dt",     -1},
  {"hx711_sck",    -1},
  {"exp_int",      -1},
  {"tc_sck",       -1},
  {"tc_mosi",      -1},
  {"tc_miso",      -1},
  {"tc_cs",        -1},
  {"rtd_cs",       -1},
  {"adc_cs",       -1},
  {"i2s_bclk",     -1},
  {"i2s_ws",       -1},
  {"i2s_din",      -1},
  {"i2s_dout",     -1},
};

// ESP32-C6: GPIO 0-23 (24-30 flash), ADC1 = GPIO 0-6, USB = 12/13.
static const BoardPinDef C6_PINS[] = {
  {"i2c_sda",      6},
  {"i2c_scl",      7},
  {"onewire",      4},
  {"rain",         5},
  {"anem",         3},
  {"vane",         0},
  {"pms_rx",       18},
  {"pms_tx",       19},
  {"sdmmc_clk",    -1},
  {"sdmmc_cmd",    -1},
  {"sdmmc_d0",     -1},
  {"sd_cs",        10},
  {"sd_sck",       11},
  {"sd_mosi",      20},
  {"sd_miso",      21},
  {"led",          8},
  {"button",       9},
  {"rgb",          2},
  {"gps_rx",       16},
  {"gps_tx",       17},
  {"rs485_rx",     22},
  {"rs485_tx",     23},
  {"rs485_de",     15},
  {"as3935_irq",   14},
  {"soil_adc",     1},
  {"pyrano_adc",   -1},
  {"scatter_adc",  -1},
  {"gas_adc",      -1},
  {"gas_adc2",     -1},
  {"par_adc",      -1},
  {"leaf_adc",     -1},
  {"acs_adc",      -1},
  {"radar_rx",     -1},
  {"radar_tx",     -1},
  {"dht",          -1},
  {"us_trig",      -1},
  {"us_echo",      -1},
  {"hx711_dt",     -1},
  {"hx711_sck",    -1},
  {"exp_int",      -1},
  {"tc_sck",       -1},
  {"tc_mosi",      -1},
  {"tc_miso",      -1},
  {"tc_cs",        -1},
  {"rtd_cs",       -1},
  {"adc_cs",       -1},
  {"i2s_bclk",     -1},
  {"i2s_ws",       -1},
  {"i2s_din",      -1},
  {"i2s_dout",     -1},
};

static const char* NOTE_S3 =
  "Ten sam układ pinów co płytka stacji (include/pins.h) - wszystkie czujniki, "
  "karta SD (SDMMC), GPS, PMS5003 i pierścień LED. Gotowej binarki węzła dla 16 MB "
  "nie ma w wydaniu (są trzy pliki: master, N8R4 i C3) - zbuduj ją sam: "
  "pio run -e esp32s3-node, albo wgraj plik master-16mb i na stronie www przestaw "
  "rolę RS485 na węzeł.";
static const char* NOTE_S3_8 =
  "Piny identyczne jak na N16R8 - różni się tylko pamięć (8 MB flash, PSRAM 8 MB "
  "octal). Wydanie nie zawiera binarki dla tej wersji - zbuduj ją sam: "
  "pio run -e esp32s3-node-8mb.";
static const char* NOTE_S3_QUAD =
  "Zalecany drugi ESP: 8 MB flash, PSRAM quad 2/4 MB. Piny identyczne jak na N16R8 "
  "- do wszystkich czujników i wysyłania po RS485 to wystarcza. Różni się tylko "
  "pamięć i plik firmware (qio_qspi).";
static const char* NOTE_C3 =
  "Węzeł na wiatr: AS5600 (wiatrowskaz) na I2C SDA 8 / SCL 9 i kontaktron anemometru "
  "na GPIO 5. Deszczomierz jest wyłączony (siedzi na osobnej płytce - kolejny węzeł), "
  "podobnie wiatrowskaz analogowy, bo kierunek daje AS5600. Wyłączone zostają też LED "
  "statusu (GPIO 8) i przycisk BOOT (GPIO 9), bo te piny zajmuje I2C. Karta SD, GPS, "
  "PMS5003, AS3935 i RTC na osobnej magistrali są niedostępne na tej płytce (-1) - "
  "do nich potrzebna większa płytka.";
static const char* NOTE_C6 =
  "Szablon kompletny (I2C, OneWire, deszcz, wiatr, wiatrowskaz, gleba, karta SD "
  "na SPI, GPS, PMS5003, RS485, AS3935). Brak binarki: ESP32-C6 wymaga Arduino "
  "core 3.x, projekt buduje się na core 2.0.17 (S3/C3).";

static const BoardProfile PROFILES[] = {
  {"s3n16r8", "ESP32-S3 WROOM-1 N16R8 - 16 MB flash, PSRAM 8 MB (octal)", "S3 N16R8",
   "esp32s3", 16, "8 MB octal (OPI)", "esp32s3-node", NOTE_S3, false, nullptr, 0},

  {"s3n8r8", "ESP32-S3 WROOM-1 N8R8 - 8 MB flash, PSRAM 8 MB (octal)", "S3 N8R8",
   "esp32s3", 8, "8 MB octal (OPI)", "esp32s3-node-8mb", NOTE_S3_8, false, nullptr, 0},

  {"s3n8r4", "ESP32-S3 WROOM-1 N8R4 / N8R2 - 8 MB flash, PSRAM quad", "S3 N8R4",
   "esp32s3", 8, "4 / 2 MB quad (QSPI)", "esp32s3-node-8mb-quad", NOTE_S3_QUAD, true, nullptr, 0},

  {"c3mini", "ESP32-C3 SuperMini - 4 MB flash, bez PSRAM", "C3 SuperMini",
   "esp32c3", 4, "brak PSRAM", "esp32c3-node", NOTE_C3, true,
   C3_PINS, sizeof(C3_PINS) / sizeof(C3_PINS[0])},

  {"c6", "ESP32-C6 - 4 MB flash, bez PSRAM", "C6",
   "esp32c6", 4, "brak PSRAM", "-", NOTE_C6, false,
   C6_PINS, sizeof(C6_PINS) / sizeof(C6_PINS[0])},
};

static const size_t PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

// -------------------------------------------------------------
//  Fizyczny układ pinów - kolejność i podpisy jak na laminacie
//
//  ESP32-S3-DevKitC-1 v1.1 wg dokumentacji Espressif (sekcja
//  "Pin Layout"): J1 = lewa kolumna, J3 = prawa kolumna.
//    J1: 3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46,
//        9, 10, 11, 12, 13, 14, 5V, G
//    J3: G, TX(43), RX(44), 1, 2, 42, 41, 40, 39, 38, 37, 36,
//        35, 0, 45, 48, 47, 21, 20, 19, G, G
//  GPIO 35-37 są na złączu, ale na N16R8/N8R8 zajmuje je PSRAM
//  (octal) - dlatego są oznaczone jako zarezerwowane.
// -------------------------------------------------------------
static const BoardPad PADS_S3[] = {
  // --- lewa kolumna (J1), od góry do dołu ---
  {1,  -1, "3V3", "zasilanie 3,3 V"},
  {1,  -1, "3V3", "zasilanie 3,3 V"},
  {1,  -1, "RST", "reset (EN)"},
  {1,   4, "4",   nullptr},
  {1,   5, "5",   nullptr},
  {1,   6, "6",   nullptr},
  {1,   7, "7",   nullptr},
  {1,  15, "15",  nullptr},
  {1,  16, "16",  nullptr},
  {1,  17, "17",  nullptr},
  {1,  18, "18",  nullptr},
  {1,   8, "8",   nullptr},
  {1,   3, "3",   nullptr},
  {1,  46, "46",  nullptr},
  {1,   9, "9",   nullptr},
  {1,  10, "10",  nullptr},
  {1,  11, "11",  nullptr},
  {1,  12, "12",  nullptr},
  {1,  13, "13",  nullptr},
  {1,  14, "14",  nullptr},
  {1,  -1, "5V",  "5 V (USB / zasilanie)"},
  {1,  -1, "G",   "masa"},
  // --- prawa kolumna (J3), od góry do dołu ---
  {2,  -1, "G",   "masa"},
  {2,  43, "TX",  nullptr},
  {2,  44, "RX",  nullptr},
  {2,   1, "1",   nullptr},
  {2,   2, "2",   nullptr},
  {2,  42, "42",  nullptr},
  {2,  41, "41",  nullptr},
  {2,  40, "40",  nullptr},
  {2,  39, "39",  "RS485 RX stacji - do węzła C3 GPIO 21 (TX węzła)"},
  {2,  38, "38",  "RS485 TX stacji - do węzła C3 GPIO 20 (RX węzła)"},
  {2,  37, "37",  nullptr},
  {2,  36, "36",  nullptr},
  {2,  35, "35",  nullptr},
  {2,   0, "0",   nullptr},
  {2,  45, "45",  nullptr},
  {2,  48, "48",  nullptr},
  {2,  47, "47",  nullptr},
  {2,  21, "21",  nullptr},
  {2,  20, "20",  nullptr},
  {2,  19, "19",  nullptr},
  {2,  -1, "G",   "masa"},
  {2,  -1, "G",   "masa"},
};

// ESP32-C3 SuperMini: dwie kolumny po 8 pinów, wyprowadzone
// GPIO 0-10, 20, 21 (18/19 to natywne USB, bez wyprowadzenia).
static const BoardPad PADS_C3[] = {
  {1,  -1, "5V",  "5 V (USB / zasilanie)"},
  {1,  -1, "G",   "masa"},
  {1,  -1, "3V3", "zasilanie 3,3 V"},
  {1,   4, "4",   "OneWire - DS18B20 (dowolnie wiele równolegle)"},
  {1,   3, "3",   nullptr},
  {1,   2, "2",   "pierścień LED RGB (WS2812B)"},
  {1,   1, "1",   "sonda gleby (ADC1)"},
  {1,   0, "0",   nullptr},
  {2,   5, "5",   "anemometr - kontaktron / Hall (impulsy na obrót)"},
  {2,   6, "6",   nullptr},
  {2,   7, "7",   nullptr},
  {2,   8, "8",   "I2C SDA - AS5600 (wiatrowskaz), BME280, SHT4x, BMP581..."},
  {2,   9, "9",   "I2C SCL - wspólna dla czujników I2C"},
  {2,  10, "10",  "RS485 DE/RE (kierunek nadawania)"},
  {2,  20, "20",  "RS485 RX węzła - do stacji GPIO 38 (TX stacji)"},
  {2,  21, "21",  "RS485 TX węzła - do stacji GPIO 39 (RX stacji)"},
};

static const BoardLayout LAYOUT_S3 = {
  "s3-devkitc1", "ESP32-S3-DevKitC-1", PADS_S3,
  (uint16_t)(sizeof(PADS_S3) / sizeof(PADS_S3[0]))
};
static const BoardLayout LAYOUT_C3 = {
  "c3-supermini", "ESP32-C3 SuperMini", PADS_C3,
  (uint16_t)(sizeof(PADS_C3) / sizeof(PADS_C3[0]))
};

const BoardLayout* boards::layoutForChip(const char* chipId) {
  if (!chipId || !*chipId) return nullptr;
  if (strcmp(chipId, "esp32s3") == 0) return &LAYOUT_S3;
  if (strcmp(chipId, "esp32c3") == 0) return &LAYOUT_C3;
  return nullptr;   // ESP32-C6 - układ niepotwierdzony, strona pokaże listę GPIO
}

const BoardLayout* boards::layout() { return layoutForChip(chipId()); }

size_t boards::count() { return PROFILE_COUNT; }

const BoardProfile& boards::at(size_t i) {
  return PROFILES[i < PROFILE_COUNT ? i : 0];
}

const BoardProfile* boards::byId(const char* id) {
  if (!id || !*id) return nullptr;
  for (size_t i = 0; i < PROFILE_COUNT; i++)
    if (strcmp(PROFILES[i].id, id) == 0) return &PROFILES[i];
  return nullptr;
}

const char* boards::chipName() {
  switch (kChip) {
    case CHIP_C3: return "ESP32-C3";
    case CHIP_C6: return "ESP32-C6";
    default:      return "ESP32-S3";
  }
}

const char* boards::chipId() {
  switch (kChip) {
    case CHIP_C3: return "esp32c3";
    case CHIP_C6: return "esp32c6";
    default:      return "esp32s3";
  }
}

// Płytka wykrywana z chipu, rozmiaru flash i PSRAM - dzięki temu
// jeden plik firmware wie, na czym pracuje i poprawnie się raportuje.
const BoardProfile& boards::current() {
  static const BoardProfile* cached = nullptr;
  if (cached) return *cached;

#if defined(CONFIG_IDF_TARGET_ESP32C3)
  cached = byId("c3mini");
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  cached = byId("c6");
#else
  const uint32_t flashMb = (uint32_t)(ESP.getFlashChipSize() / (1024UL * 1024UL));
  const uint32_t psramMb = (uint32_t)(ESP.getPsramSize() / (1024UL * 1024UL));
  if (flashMb >= 16)             cached = byId("s3n16r8");
  else if (psramMb >= 8)         cached = byId("s3n8r8");
  else                           cached = byId("s3n8r4");
#endif

  if (!cached) cached = &PROFILES[0];
  return *cached;
}

bool boards::isCurrent(const char* id) {
  const BoardProfile* p = byId(id);
  return p && strcmp(p->id, current().id) == 0;
}

int boards::defaultPinOf(const BoardProfile& p, const char* key, int fallback) {
  if (p.pins) {
    for (uint8_t i = 0; i < p.pinCount; i++)
      if (strcmp(p.pins[i].key, key) == 0) return p.pins[i].gpio;
  }
  return fallback;
}

int boards::defaultPin(const char* key, int fallback) {
  return defaultPinOf(current(), key, fallback);
}

// ------------------------------------------------------------
//  Co wolno podłączyć na danym chipie
// ------------------------------------------------------------
const char* boards::reservedReason(int gpio) {
  switch (kChip) {
    case CHIP_C3:
      if (gpio >= 11 && gpio <= 17) return "GPIO 11-17 zajęte przez flash (ESP32-C3)";
      if (gpio > 21)                return "GPIO nie istnieje w ESP32-C3 (dostępne 0-10 i 18-21)";
      break;

    case CHIP_C6:
      if (gpio >= 24 && gpio <= 30) return "GPIO 24-30 zajęte przez flash (ESP32-C6)";
      if (gpio > 30)                return "GPIO nie istnieje w ESP32-C6 (dostępne 0-23)";
      break;

    default:
      if (gpio >= 22 && gpio <= 25) return "GPIO 22-25 nie istnieje w ESP32-S3";
      if (gpio >= 26 && gpio <= 37) return "GPIO 26-37 zajęte przez flash/PSRAM (N16R8)";
      break;
  }
  return nullptr;
}

const char* boards::warningReason(const char* key, int gpio) {
  switch (kChip) {
    case CHIP_C3:
      if (gpio == 18 || gpio == 19) return "GPIO 18/19 to natywne USB - port szeregowy przestanie działać";
      if (gpio == 20 || gpio == 21) return "GPIO 20/21 to UART0 - domyślne piny magistrali RS485";
      if (gpio == 2 || gpio == 8 || gpio == 9) return "GPIO 2/8/9 to pin strapping - poziom przy starcie ma znaczenie";
      break;

    case CHIP_C6:
      if (gpio == 12 || gpio == 13) return "GPIO 12/13 to natywne USB - port szeregowy przestanie działać";
      if (gpio == 4 || gpio == 5 || gpio == 8 || gpio == 9 || gpio == 15)
        return "GPIO 4/5/8/9/15 to pin strapping - poziom przy starcie ma znaczenie";
      break;

    default:
      if (gpio == 19 || gpio == 20) return "GPIO 19/20 to natywne USB - port szeregowy przestanie działać";
      if (gpio == 43 || gpio == 44) return "GPIO 43/44 to UART0 (konsola)";
      if (gpio == 45 || gpio == 46) return "GPIO 45/46 to pin strapping - poziom przy starcie ma znaczenie";
      if (gpio == 3)  return "GPIO 3 to pin strapping (JTAG)";
      if (gpio == 0 && (!key || strcmp(key, "button") != 0)) return "GPIO 0 to pin strapping (BOOT)";
      break;
  }
  return nullptr;
}

const char* boards::gpioNote(int gpio) {
  switch (kChip) {
    case CHIP_C3:
      if (gpio >= 11 && gpio <= 17) return "flash - nie używać";
      if (gpio == 18 || gpio == 19) return "natywne USB";
      if (gpio == 20 || gpio == 21) return "UART0";
      if (gpio == 2 || gpio == 8 || gpio == 9) return "strapping";
      if (gpio >= 0 && gpio <= 4) return "ADC1";
      if (gpio == 5) return "ADC2 (nie działa z Wi-Fi)";
      return "";

    case CHIP_C6:
      if (gpio >= 24 && gpio <= 30) return "flash - nie używać";
      if (gpio == 12 || gpio == 13) return "natywne USB";
      if (gpio == 4 || gpio == 5 || gpio == 8 || gpio == 9 || gpio == 15) return "strapping";
      if (gpio >= 0 && gpio <= 6) return "ADC1";
      return "";

    default:
      if (gpio >= 26 && gpio <= 37) return "flash / PSRAM - nie używać";
      if (gpio == 19 || gpio == 20) return "natywne USB";
      if (gpio == 43 || gpio == 44) return "UART0";
      if (gpio == 45 || gpio == 46) return "strapping";
      if (gpio == 0) return "strapping (BOOT)";
      if (gpio == 3) return "strapping";
      if (gpio >= 1 && gpio <= 10) return "ADC1";
      if (gpio >= 11 && gpio <= 20) return "ADC2";
      return "";
  }
}

bool boards::pinExists(int gpio) {
  if (gpio < 0) return false;
  switch (kChip) {
    case CHIP_C3: return gpio <= 21;
    case CHIP_C6: return gpio <= 30;
    default:      return gpio <= 48 && !(gpio >= 22 && gpio <= 25);
  }
}

int boards::maxPin() {
  switch (kChip) {
    case CHIP_C3: return 21;
    case CHIP_C6: return 30;
    default:      return 48;
  }
}

bool boards::analogOk(int gpio) {
  switch (kChip) {
    case CHIP_C3: return gpio >= 0 && gpio <= 4;
    case CHIP_C6: return gpio >= 0 && gpio <= 6;
    default:      return gpio >= 1 && gpio <= 10;
  }
}

const char* boards::analogHint() {
  switch (kChip) {
    case CHIP_C3: return "GPIO 0-4 (ADC1)";
    case CHIP_C6: return "GPIO 0-6 (ADC1)";
    default:      return "GPIO 1-10 (ADC1)";
  }
}

const char* boards::adc2Note() {
  switch (kChip) {
    case CHIP_C3: return "ADC2 to GPIO 5 - przy włączonym Wi-Fi nie działa";
    case CHIP_C6: return "ESP32-C6 ma tylko ADC1";
    default:      return "ADC2 (GPIO 11-20) przy włączonym Wi-Fi nie działa";
  }
}

// ------------------------------------------------------------
//  JSON dla strony www
// ------------------------------------------------------------
static bool chipIsC3(const BoardProfile& p) { return strcmp(p.chip, "esp32c3") == 0; }
static bool chipIsC6(const BoardProfile& p) { return strcmp(p.chip, "esp32c6") == 0; }

// Zakres ADC1 i najwyższy GPIO liczone dla płytki z szablonu (nie dla
// tej, na której akurat pracujemy) - master pokazuje szablony wszystkich płytek.
static const char* analogHintOf(const BoardProfile& p) {
  if (chipIsC3(p)) return "GPIO 0-4 (ADC1)";
  if (chipIsC6(p)) return "GPIO 0-6 (ADC1)";
  return "GPIO 1-10 (ADC1)";
}

static int maxPinOf(const BoardProfile& p) {
  if (chipIsC3(p)) return 21;
  if (chipIsC6(p)) return 30;
  return 48;
}

static size_t fsFileSize(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

// Rozmiar w MB zaokrąglony do najbliższej - ESP.getPsramSize() zwraca 8189 kB,
// więc dzielenie całkowite pokazywałoby 7 MB dla modułu 8 MB.
static uint32_t sizeToMb(size_t bytes) {
  return (uint32_t)((bytes + 512UL * 1024UL) / (1024UL * 1024UL));
}

String boards::defsJson(const BoardProfile& p) {
  JsonDocument d;
  JsonObject o = d.to<JsonObject>();
  for (size_t i = 0; i < pinMapRoleCount(); i++) {
    const char* key = pinMapRoleKey(i);
    if (!key) continue;
    o[key] = defaultPinOf(p, key, pinMapRoleDef(i));
  }
  String out;
  serializeJson(d, out);
  return out;
}

String boards::currentJson() {
  const BoardProfile& p = current();
  JsonDocument d;
  JsonObject o = d.to<JsonObject>();
  o["board"] = p.id;
  o["board_label"] = p.label;
  o["board_short"] = p.shortName;
  o["chip"] = chipId();
  o["chip_name"] = chipName();
  o["flash_mb"] = sizeToMb(ESP.getFlashChipSize());
  o["psram_mb"] = sizeToMb(ESP.getPsramSize());
  String out;
  serializeJson(d, out);
  return out;
}

String boards::catalogJson() {
  JsonDocument d;
  JsonObject root = d.to<JsonObject>();
  root["ok"] = true;
  root["current"] = current().id;
  root["chip"] = chipId();
  root["chip_name"] = chipName();
  root["flash_mb"] = sizeToMb(ESP.getFlashChipSize());
  root["psram_mb"] = sizeToMb(ESP.getPsramSize());
  root["analog_hint"] = analogHint();
  root["adc2_note"] = adc2Note();
  root["max_pin"] = maxPin();

  JsonArray arr = root["boards"].to<JsonArray>();
  for (size_t i = 0; i < PROFILE_COUNT; i++) {
    const BoardProfile& p = PROFILES[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["label"] = p.label;
    o["short"] = p.shortName;
    o["chip"] = p.chip;
    o["flash_mb"] = p.flashMb;
    o["ram"] = p.ram;
    o["env"] = p.env;
    o["note"] = p.note;
    o["buildable"] = p.buildable;
    o["current"] = (strcmp(p.id, current().id) == 0);
    o["analog_hint"] = analogHintOf(p);
    o["max_pin"] = maxPinOf(p);

    char path[48];
    snprintf(path, sizeof(path), "/node/%s.bin", p.id);
    bool haveFull = p.buildable && LittleFS.exists(path);
    o["have_full"] = haveFull;
    o["size_full"] = haveFull ? (uint32_t)fsFileSize(path) : 0;
    snprintf(path, sizeof(path), "/node/%s-ota.bin", p.id);
    bool haveOta = p.buildable && LittleFS.exists(path);
    o["have_ota"] = haveOta;
    o["size_ota"] = haveOta ? (uint32_t)fsFileSize(path) : 0;

    // Gotowe binarki "z adresem" (np. /node/c3mini-addr3.bin) - każda ma
    // wkompilowany inny adres fabryczny, więc kilka świeżych węzłów działa
    // od razu bez kolizji na magistrali. Lista pojawia się w tabeli firmware.
    JsonArray addrs = o["addrs"].to<JsonArray>();
    for (int a = 2; a <= 32; a++) {
      snprintf(path, sizeof(path), "/node/%s-addr%d.bin", p.id, a);
      if (p.buildable && LittleFS.exists(path)) {
        JsonObject ao = addrs.add<JsonObject>();
        ao["addr"] = a;
        ao["size"] = (uint32_t)fsFileSize(path);
      }
    }

    JsonObject defs = o["defs"].to<JsonObject>();
    for (size_t r = 0; r < pinMapRoleCount(); r++) {
      const char* key = pinMapRoleKey(r);
      if (!key) continue;
      defs[key] = defaultPinOf(p, key, pinMapRoleDef(r));
    }
  }

  String out;
  serializeJson(d, out);
  return out;
}
