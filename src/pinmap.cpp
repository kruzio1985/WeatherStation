/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "pinmap.h"
#include "board.h"
#include "pins.h"
#include "config.h"
#include "syslog.h"
#include <string.h>

// ------------------------------------------------------------
//  Mapa pinów. Wartości z include/pins.h są tylko domyślne -
//  użytkownik może je zmienić na stronie www (zakładka "Piny").
// ------------------------------------------------------------

PinMap pinMap;

static const PinRole ROLES[] = {
  // --- I2C: BME280 / BH1750 ---
  {"i2c_sda",    "I2C - SDA",          "I2C", "Magistrala I2C (SDA): BME280/680/688, SHT3x, BMP280/581, SCD4x, SGP30, SPS30, BH1750, VEML6075, AS5600, INA2xx", PIN_SDA, false},
  {"i2c_scl",    "I2C - SCL",          "I2C", "Magistrala I2C (SCL) - wspólna dla czujników z linii SDA; pull-up 4,7 kΩ do 3V3", PIN_SCL, false},

  // --- Czujniki zewnętrzne ---
  {"onewire",    "DS18B20 (OneWire)",  "Czujniki zewnętrzne", "Magistrala OneWire - do 8× DS18B20 (gleba, woda); rezystor 4,7 kΩ do 3V3", PIN_ONEWIRE, false},
  {"rain",       "Deszczomierz",       "Czujniki zewnętrzne", "Przechyłowy / kontaktron / Hall - zwieranie do GND; FC-37 i YL-83 na wejściu ADC",   PIN_RAIN,  false},
  {"anem",       "Anemometr",          "Czujniki zewnętrzne", "Kontaktron / Hall (A3144, SS49E) - impulsy na obrót",  PIN_ANEM,  false},
  {"vane",       "Wiatrowskaz",        "Czujniki zewnętrzne", "Potencjometr na ADC1 albo enkoder AS5600 / AS5048 przez I2C",         PIN_VANE,  true},

  // --- PMS5003 (jakość powietrza, UART2) ---
  {"pms_rx",     "PMS5003 - RX",       "Jakość powietrza", "RX ESP32 <- linia TX czujnika pyłu (PMS5003, SDS011, SPS30, SEN5x)",  PIN_PMS_RX, false},
  {"pms_tx",     "PMS5003 - TX",       "Jakość powietrza", "TX ESP32 -> linia RX czujnika PMS",  PIN_PMS_TX, false},

  // --- Karta microSD: SDMMC 1-bit (opcja, domyślnie wyłączona) ---
  {"sdmmc_clk",  "SD SDMMC - CLK",     "Karta SD", "Tryb SDMMC 1-bit - zegar; domyślnie -1 (wyłączony), bo karta pracuje po SPI", PIN_SDMMC_CLK, false},
  {"sdmmc_cmd",  "SD SDMMC - CMD",     "Karta SD", "Tryb SDMMC 1-bit - komendy",                    PIN_SDMMC_CMD, false},
  {"sdmmc_d0",   "SD SDMMC - D0",      "Karta SD", "Tryb SDMMC 1-bit - dane",                       PIN_SDMMC_D0,  false},

  // --- Karta microSD: SPI (podstawowy) ---
  {"sd_cs",      "SD SPI - CS",        "Karta SD", "Tryb SPI - wybór karty (CS); -1 wyłącza tryb SPI", PIN_SD_CS,   false},
  {"sd_sck",     "SD SPI - SCK",       "Karta SD", "Tryb SPI - zegar (SCK) - firmware przekazuje go do SD.begin()", PIN_SD_SCK,  false},
  {"sd_mosi",    "SD SPI - MOSI",      "Karta SD", "Tryb SPI - dane do karty (MOSI)",            PIN_SD_MOSI, false},
  {"sd_miso",    "SD SPI - MISO",      "Karta SD", "Tryb SPI - dane z karty (MISO)",             PIN_SD_MISO, false},

  // --- Płytka ---
  {"led",        "LED statusu",        "Płytka", "Miga 1 Hz gdy stacja działa; -1 wyłącza", PIN_LED,    false},
  {"button",     "Przycisk",           "Płytka", "Przytrzymanie 3 s = reset do ustawień fabrycznych", PIN_BUTTON, false},
  {"rgb",        "Pierścień LED RGB",  "Płytka", "WS2812B / RGBIC 36 px - jeden przewód danych; 5V i GND z płytki",  PIN_RGB,  false},

  // --- GPS (odbiornik NMEA 0183 na UART1) ---
  {"gps_rx",     "GPS - RX",           "GPS", "RX ESP32 <- TX odbiornika (NEO-6M/7M/8M, ATGM336H, L76K); NMEA 0183, 9600 Bd; -1 wyłącza GPS", PIN_GPS_RX, false},
  {"gps_tx",     "GPS - TX",           "GPS", "TX ESP32 -> RX odbiornika; potrzebne tylko do zmiany ustawień modułu (u-center) - można zostawić -1", PIN_GPS_TX, false},

  // --- Magistrala RS485 (drugi ESP z zewnętrznymi czujnikami) ---
  // Opis zależy od roli płytki: master pisze, do którego pinu węzła idzie
  // przewód, a węzeł - do którego pinu mastera (żeby nie trzeba było tego
  // szukać w README przy podłączaniu kolejnej płytki).
#if STACJA_ROLE_MASTER
  {"rs485_rx",   "RS485 - RX (od węzła)",  "Magistrala RS485", "Odbiór od węzła. Przewód z tego pinu (RX mastera 39) łączymy z TX węzła: C3 SuperMini GPIO 21. Trzeci przewód to GND (wspólna masa)", PIN_RS485_RX, false},
  {"rs485_tx",   "RS485 - TX (do węzła)",  "Magistrala RS485", "Nadawanie do węzła. Przewód z tego pinu (TX mastera 38) łączymy z RX węzła: C3 SuperMini GPIO 20", PIN_RS485_TX, false},
  {"rs485_de",   "RS485 - DE/RE",          "Magistrala RS485", "Tylko dla modułu z konwerterem RS485 (MAX485/SP3485): kierunek nadawania. Przy połączeniu bezpośrednim (TX-RX-GND) ten pin zostaw wolny (-1)", PIN_RS485_DE, false},
#else
  {"rs485_rx",   "RS485 - RX (od mastera)", "Magistrala RS485", "Odbiór od mastera. Przewód z tego pinu (RX 20) łączymy z TX mastera: GPIO 38. Trzeci przewód to GND (wspólna masa)", PIN_RS485_RX, false},
  {"rs485_tx",   "RS485 - TX (do mastera)", "Magistrala RS485", "Nadawanie do mastera. Przewód z tego pinu (TX 21) łączymy z RX mastera: GPIO 39", PIN_RS485_TX, false},
  {"rs485_de",   "RS485 - DE/RE",          "Magistrala RS485", "Tylko dla modułu z konwerterem RS485 (MAX485/SP3485): kierunek nadawania. Przy połączeniu bezpośrednim (TX-RX-GND) ten pin zostaw wolny (-1)", PIN_RS485_DE, false},
#endif

  // --- Detektor wyładowań (I2C 0x03) ---
  {"as3935_irq", "AS3935 - IRQ",       "Burza", "Przerwanie z AS3935: wyładowanie / zakłócenie / szum; I2C 0x03, zasilanie 3V3", PIN_AS3935_IRQ, false},

  // --- Czujniki analogowe (ADC) ---
  {"soil_adc",   "Gleba - wilgotność", "Czujniki analogowe", "Sonda pojemnościowa (capacitive v1.2, VH400) - wyjście AOUT; tylko GPIO 1-10 (ADC1)", PIN_SOIL_ADC, true},
  {"pyrano_adc", "Pyranometr / nasłonecznienie", "Czujniki analogowe", "Pyranometr (ML-01, 0-3 V) albo ogniwo słoneczne - daje W/m² i zachmurzenie; tylko ADC1 (wolny jest np. GPIO 10 - wtedy wyłącz kartę SD w trybie SPI na pinie 10)", PIN_PYRANO_ADC, true},
  {"scatter_adc", "Widzialność (rozproszenie światła)", "Czujniki analogowe", "Analogowy czujnik mgły/widzialności. UWAGA: GPIO 11-20 to ADC2, który przy włączonym Wi-Fi nie działa - użyj ADS1115 na I2C", PIN_SCATTER_ADC, true},
  {"gas_adc",    "Czujniki gazów (MQ-x, MiCS)", "Czujniki analogowe", "MQ-2/3/4/5/6/7/8/9/131/135, MiCS-4514/6814, TGS - wyjście analogowe przez dzielnik nap. (wolny pin ADC1)", PIN_GAS_ADC, true},
  {"gas_adc2",   "Czujniki gazów 2 (drugi MQ-x)", "Czujniki analogowe", "Drugi czujnik gazów pracujący równolegle (np. MQ-7 na CO + MQ-135); osobny wolny pin ADC1", PIN_GAS_ADC2, true},
  {"par_adc",    "PAR / światło do fotosyntezy", "Czujniki analogowe", "Czujnik PAR (S2-131, fotodioda + ADS1115) - ilość światła użyteczna dla roślin, µmol/m²/s; wolny pin ADC1", PIN_PAR_ADC, true},
  {"leaf_adc",   "Mokrość liścia / oblodzenie", "Czujniki analogowe", "Czujnik mokrości liścia (analog) albo czujnik oblodzenia - wolny pin ADC1", PIN_LEAF_ADC, true},
  {"acs_adc",    "Prąd - ACS712 / ACS758", "Czujniki analogowe", "Przekładnik prądowy Halla (0-5 A / 30 A): pomiar prądu i mocy panelu albo grzałki; wolny pin ADC1", PIN_ACS_ADC, true},

  // --- Czujniki cyfrowe na pojedynczych liniach ---
  {"dht",        "DHT11 / DHT22 / AM2302", "Czujniki cyfrowe", "Jeden przewód danych + rezystor 4,7 kΩ do 3V3; odczyt co 2 s (DHT11) lub 1 s (DHT22) - przy 5 m kabla użyj DHT22", PIN_DHT, false},
  {"us_trig",    "HC-SR04 - TRIG",      "Czujniki cyfrowe", "Wyzwalanie ultradźwięków (HC-SR04, JSN-SR04T, A02YYUW) - poziom lodu, śniegu, poziom wody w studni", PIN_US_TRIG, false},
  {"us_echo",    "HC-SR04 - ECHO",      "Czujniki cyfrowe", "Odbicie ultradźwięków -> odległość; daje też poziom wody/śniegu i wykrywanie obecności", PIN_US_ECHO, false},
  {"hx711_dt",   "HX711 - DT (dane)",   "Czujniki cyfrowe", "Waga deszczomierza wagowego, tensjometr gleby, siła wiatru - przetwornik 24-bit", PIN_HX711_DT, false},
  {"hx711_sck",  "HX711 - SCK (zegar)", "Czujniki cyfrowe", "Zegar HX711; razem z DT tworzy wagę (deszczomierz wagowy, tensjometr)", PIN_HX711_SCK, false},
  {"exp_int",    "Ekspander - INT",     "Czujniki cyfrowe", "Przerwanie z MCP23017 / PCF8574/8575 (rozszerzenie wejść przy braku pinów); magistrala I2C wspólna z czujnikami", PIN_EXP_INT, false},

  // --- Radar obecności / ruchu (UART 256000) ---
  {"radar_rx",   "Radar LD2410/2450 - RX", "Radar obecności", "TX radaru -> RX ESP32: raporty obecności, odległości i ruchu celu (HLK-LD2410B/C, LD2450, LD2461); zasilanie 5 V!", PIN_RADAR_RX, false},
  {"radar_tx",   "Radar LD2410/2450 - TX", "Radar obecności", "TX ESP32 -> RX radaru: zmiana ustawień modułu (bramki odległości, czułości) - można zostawić, jeśli wystarczy odczyt", PIN_RADAR_TX, false},

  // --- Termopary i RTD (SPI) ---
  {"tc_sck",     "Termopara - SCK",     "Termopary / RTD", "Zegar SPI - wspólny dla MAX6675, MAX31855 i MAX31865 (PT100/PT1000)", PIN_TC_SCK, false},
  {"tc_mosi",    "Termopara - MOSI (SDI)", "Termopary / RTD", "Dane do układu (SDI w MAX31865); MAX6675/31855 nie używają tej linii, ale magistrala jest wspólna", PIN_TC_MOSI, false},
  {"tc_miso",    "Termopara - MISO (SDO)", "Termopary / RTD", "Dane z układu (temperatura + błąd termopary/RTD)", PIN_TC_MISO, false},
  {"tc_cs",      "Termopara - CS (MAX6675/31855)", "Termopary / RTD", "Wybór układu termopary typu K/J/T/E (piec, komin, kolektor słoneczny, kompost)", PIN_TC_CS, false},
  {"rtd_cs",     "RTD PT100/PT1000 - CS (MAX31865)", "Termopary / RTD", "Osobna linia CS, bo MAX31865 dzieli SCK/MOSI/MISO z termoparą; precyzyjny pomiar (0,03 °C)", PIN_RTD_CS, false},
  {"adc_cs",     "ADC MCP3008/3208 - CS", "Termopary / RTD", "Osobna linia CS dla przetwornika MCP3008 (10 bit) / MCP3208 (12 bit) - 8 dodatkowych wejść analogowych; linie SCK/MOSI/MISO wspólne z termoparami", PIN_ADC_CS, false},

  // --- Audio / mikrofon I²S ---
  {"i2s_bclk",   "I²S - BCLK (zegar)",  "Audio / I²S", "Zegar bitowy - wspólny dla mikrofonu (INMP441, ICS-43434, SPH0645, PDM) i wzmacniacza (MAX98357A)", PIN_I2S_BCLK, false},
  {"i2s_ws",     "I²S - WS (LRCK)",     "Audio / I²S", "Zegar ramki (lewy/prawy kanał) - jedna linia dla mikrofonu i wzmacniacza", PIN_I2S_WS, false},
  {"i2s_din",    "I²S - DIN (z mikrofonu)", "Audio / I²S", "Dane z mikrofonu do ESP32 (SD mikrofonu) - poziom hałasu, wykrywanie deszczu/gradobicia, nasłuch", PIN_I2S_DIN, false},
  {"i2s_dout",   "I²S - DOUT (do wzmacniacza)", "Audio / I²S", "Dane z ESP32 do wzmacniacza MAX98357A - komunikaty głosowe i alarmy stacji", PIN_I2S_DOUT, false},
};

static const size_t ROLE_COUNT = sizeof(ROLES) / sizeof(ROLES[0]);
static_assert(ROLE_COUNT <= 64, "values_ w PinMap jest za małe");

size_t pinMapRoleCount() { return ROLE_COUNT; }
const char* pinMapRoleKey(size_t i) { return i < ROLE_COUNT ? ROLES[i].key : nullptr; }
int pinMapRoleDef(size_t i) { return i < ROLE_COUNT ? ROLES[i].def : -1; }

// Domyślny pin funkcji na płytce, na której działa firmware:
// najpierw szablon płytki (src/board.cpp), potem wartość z include/pins.h.
static int roleDef(size_t i) {
  return boards::defaultPin(ROLES[i].key, ROLES[i].def);
}

// Piny nieistniejące / zajęte przez pamięć / ryzykowne - zależne od chipu
// (ESP32-S3, ESP32-C3, ESP32-C6). Logika siedzi w src/board.cpp.
static const char* reservedReason(int gpio) { return boards::reservedReason(gpio); }
static const char* warningReason(const char* key, int gpio) { return boards::warningReason(key, gpio); }
static const char* gpioNote(int gpio) { return boards::gpioNote(gpio); }

static void addProblem(JsonArray arr, const char* level, const String& text) {
  JsonObject o = arr.add<JsonObject>();
  o["level"] = level;
  o["text"] = text;
}

void PinMap::begin() {
  for (size_t i = ROLE_COUNT; i < sizeof(values_); i++) values_[i] = -1;
  for (size_t i = 0; i < ROLE_COUNT; i++) {
    const int def = roleDef(i);
    int v = config.pinOverride(ROLES[i].key, def);
    if (v < -1 || v > boards::maxPin() || (v >= 0 && reservedReason(v))) v = def;
    values_[i] = (int8_t)v;
  }
  loaded_ = true;

  unsigned changed = 0;
  for (size_t i = 0; i < ROLE_COUNT; i++) if (values_[i] != roleDef(i)) changed++;
  LOG_I("Piny: %u funkcji, %u zmienionych względem domyślnych (%s)",
        (unsigned)ROLE_COUNT, changed, boards::current().shortName);

  String p = problems();
  if (p.length()) LOG_W("Konflikty pinów: %s", p.c_str());
}

int PinMap::count() const { return (int)ROLE_COUNT; }

const PinRole* PinMap::role(size_t i) const {
  return i < ROLE_COUNT ? &ROLES[i] : nullptr;
}

int PinMap::indexOf(const char* key) const {
  if (!key) return -1;
  for (size_t i = 0; i < ROLE_COUNT; i++)
    if (strcmp(ROLES[i].key, key) == 0) return (int)i;
  return -1;
}

const PinRole* PinMap::find(const char* key) const {
  int i = indexOf(key);
  return i < 0 ? nullptr : &ROLES[i];
}

int PinMap::pin(const char* key) const {
  int i = indexOf(key);
  if (i < 0) return -1;
  return loaded_ ? values_[i] : roleDef(i);
}

PinCheck PinMap::checkIndex(int idx, int gpio, const int8_t* values) const {
  PinCheck c;
  if (gpio < -1 || gpio > boards::maxPin()) {
    c.ok = false;
    c.message = String("Numer GPIO musi być z zakresu 0-") + boards::maxPin() +
                " na " + boards::chipName() + " (-1 wyłącza funkcję)";
    return c;
  }
  if (gpio < 0) return c;

  const char* res = reservedReason(gpio);
  if (res) { c.ok = false; c.message = res; return c; }

  if (ROLES[idx].analog && !boards::analogOk(gpio)) {
    c.ok = false;
    c.message = String("Wymaga wejścia analogowego ADC1 (") + boards::analogHint() + ")";
    return c;
  }

  for (size_t j = 0; j < ROLE_COUNT; j++) {
    if ((int)j == idx || values[j] != gpio) continue;
    c.ok = false;
    c.message = String("GPIO ") + gpio + " jest już przypisany do: " + ROLES[j].label;
    return c;
  }

  const char* w = warningReason(ROLES[idx].key, gpio);
  if (w) { c.warning = true; c.message = w; }
  return c;
}

PinCheck PinMap::check(const char* key, int gpio) const {
  int idx = indexOf(key);
  PinCheck c;
  if (idx < 0) { c.ok = false; c.message = String("Nieznana funkcja pinu: ") + key; return c; }
  return checkIndex(idx, gpio, values_);
}

bool PinMap::set(const char* key, int gpio, String& err) {
  int idx = indexOf(key);
  if (idx < 0) { err = String("Nieznana funkcja pinu: ") + key; return false; }
  PinCheck c = checkIndex(idx, gpio, values_);
  if (!c.ok) { err = c.message; return false; }
  values_[idx] = (int8_t)gpio;
  return true;
}

void PinMap::resetToDefaults() {
  for (size_t i = 0; i < ROLE_COUNT; i++) values_[i] = (int8_t)roleDef(i);
}

void PinMap::writeConfig() {
  for (size_t i = 0; i < ROLE_COUNT; i++) {
    if (values_[i] == roleDef(i)) config.removePinOverride(ROLES[i].key);
    else                          config.setPinOverride(ROLES[i].key, values_[i]);
  }
}

bool PinMap::save() {
  writeConfig();
  return config.save();
}

void PinMap::conflictsInto(JsonArray arr, const int8_t* values) const {
  for (size_t i = 0; i < ROLE_COUNT; i++) {
    const int g = values[i];
    if (g < -1) {
      addProblem(arr, "err", String(ROLES[i].label) + ": nieprawidłowy numer GPIO");
      continue;
    }
    if (g < 0) continue;

    const char* res = reservedReason(g);
    if (res) { addProblem(arr, "err", String(ROLES[i].label) + ": " + res); continue; }

    if (ROLES[i].analog && !boards::analogOk(g)) {
      addProblem(arr, "err", String(ROLES[i].label) + ": wymaga wejścia analogowego ADC1 (" +
                            boards::analogHint() + ")");
      continue;
    }

    for (size_t j = i + 1; j < ROLE_COUNT; j++) {
      if (values[j] != g) continue;
      addProblem(arr, "err", String("GPIO ") + g + " użyty dwa razy: " + ROLES[i].label +
                             " i " + ROLES[j].label);
    }

    const char* w = warningReason(ROLES[i].key, g);
    if (w) addProblem(arr, "warn", String(ROLES[i].label) + ": " + w);
  }
}

String PinMap::problems() const {
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  conflictsInto(arr, values_);
  String out;
  for (JsonVariant v : arr) {
    if (out.length()) out += "; ";
    out += v["text"].as<const char*>();
  }
  return out;
}

String PinMap::toJson() const {
  JsonDocument d;

  // Płytka, na której działa firmware - domyślne piny i zakresy GPIO
  // zależą od niej (ESP32-S3 / ESP32-C3 / ESP32-C6).
  const BoardProfile& bp = boards::current();
  d["board"]       = bp.id;
  d["board_label"] = bp.label;
  d["board_short"] = bp.shortName;
  d["chip"]        = boards::chipId();
  d["chip_name"]   = boards::chipName();
  d["analog_hint"] = boards::analogHint();
  d["adc2_note"]   = boards::adc2Note();
  d["max_pin"]     = boards::maxPin();

  JsonArray pins = d["pins"].to<JsonArray>();
  for (size_t i = 0; i < ROLE_COUNT; i++) {
    const int def = roleDef(i);
    JsonObject o = pins.add<JsonObject>();
    o["key"]     = ROLES[i].key;
    o["label"]   = ROLES[i].label;
    o["group"]   = ROLES[i].group;
    o["desc"]    = ROLES[i].desc;
    o["def"]     = def;
    o["analog"]  = ROLES[i].analog;
    o["gpio"]    = values_[i];
    o["changed"] = values_[i] != def;
  }

  JsonArray gpios = d["gpios"].to<JsonArray>();
  for (int g = 0; g <= boards::maxPin(); g++) {
    if (!boards::pinExists(g)) continue;
    JsonObject o = gpios.add<JsonObject>();
    o["gpio"] = g;
    o["note"] = gpioNote(g);
    o["free"] = true;
    for (size_t i = 0; i < ROLE_COUNT; i++) {
      if (values_[i] != g) continue;
      o["role"] = ROLES[i].label;
      o["free"] = false;
      break;
    }
  }

  JsonArray pr = d["problems"].to<JsonArray>();
  conflictsInto(pr, values_);
  d["ok"] = pr.size() == 0;

  // Fizyczny układ pinów płytki (dwie kolumny złącza) - strona www
  // pokazuje dzięki temu listę w kolejności takiej, jak na laminacie.
  const BoardLayout* lo = boards::layout();
  if (lo) {
    JsonObject lay = d["layout"].to<JsonObject>();
    lay["kind"]  = lo->kind;
    lay["title"] = lo->title;
    lay["board"] = bp.shortName;
    JsonArray pads = lay["pads"].to<JsonArray>();
    uint8_t posIn[3] = {0, 0, 0};
    for (uint16_t i = 0; i < lo->padCount; i++) {
      const BoardPad& pad = lo->pads[i];
      const uint8_t col = (pad.col == 2) ? 2 : 1;
      posIn[col]++;
      JsonObject o = pads.add<JsonObject>();
      o["col"]      = col;
      o["pos"]      = posIn[col];
      o["label"]    = pad.label;
      o["gpio"]     = pad.gpio;
      o["note"]     = pad.note ? pad.note : (pad.gpio >= 0 ? gpioNote(pad.gpio) : "");
      o["reserved"] = pad.gpio >= 0 && reservedReason(pad.gpio) != nullptr;
      o["adc"]      = pad.gpio >= 0 && boards::analogOk(pad.gpio);
    }
  }

  String out;
  serializeJson(d, out);
  return out;
}

bool PinMap::applyJson(const char* json, size_t len, String& err) {
  JsonDocument d;
  DeserializationError e = deserializeJson(d, json, len);
  if (e) { err = String("Nieprawidłowy JSON: ") + e.c_str(); return false; }

  JsonObject src;
  if (d["pins"].is<JsonObject>())  src = d["pins"].as<JsonObject>();
  else if (d.is<JsonObject>())     src = d.as<JsonObject>();
  if (!src) { err = "Brak sekcji 'pins' w danych"; return false; }

  int8_t next[ROLE_COUNT];
  memcpy(next, values_, ROLE_COUNT);

  // Najpierw wszystkie wpisy (walidacja zbiorcza na końcu, żeby wykryć też
  // duplikaty powstające dopiero po zmianie kilku pinów naraz).
  for (JsonPair kv : src) {
    int idx = indexOf(kv.key().c_str());
    if (idx < 0) { err = String("Nieznana funkcja pinu: ") + kv.key().c_str(); return false; }
    if (!kv.value().is<int>()) {
      err = String("Wartość dla '") + kv.key().c_str() + "' musi być liczbą";
      return false;
    }
    int gpio = kv.value().as<int>();
    if (gpio < -1 || gpio > boards::maxPin()) {
      err = String(ROLES[idx].label) + ": numer GPIO musi być z zakresu 0-" +
            boards::maxPin() + " na " + boards::chipName() + " (-1 wyłącza)";
      return false;
    }
    next[idx] = (int8_t)gpio;
  }

  // Pełna walidacja kompletu przed zapisem - albo wszystko, albo nic.
  for (size_t i = 0; i < ROLE_COUNT; i++) {
    PinCheck c = checkIndex(i, next[i], next);
    if (!c.ok) { err = String(ROLES[i].label) + ": " + c.message; return false; }
  }

  memcpy(values_, next, ROLE_COUNT);
  save();
  return true;
}
