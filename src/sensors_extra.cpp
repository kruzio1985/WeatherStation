/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "sensors.h"
#include "config.h"
#include "pinmap.h"
#include "syslog.h"
#include "drv_i2c.h"
#include "drv_pm.h"
#include "drv_table.h"
#include <Wire.h>
#include <math.h>

// =============================================================
//  Dodatkowe czujniki (dokładane modułowo).
//
//  Zasada wykrywania sprzętu:
//    * sterownik I2C musi potwierdzić układ (WHO AM I / odpowiedź na
//      komendę + CRC) - dopiero wtedy kanał jest "skonfigurowany",
//    * kanał analogowy (gleba, pyranometr) nie może "pływać" - wolny
//      pin bez czujnika poznajemy po tym, że podciągnięcie do VCC/GND
//      zmienia odczyt o prawie cały zakres,
//    * wartość pojawia się dopiero po pierwszej realnej próbce.
//  Bez tego kanały pokazywałyby dane z nieistniejących czujników.
// =============================================================

// Klucze kalibracji w NVS (config.extraF / setExtraF)
#define KEY_SOIL_DRY  "soil_dry_v"     // napięcie czujnika gleby w suchym [V]
#define KEY_SOIL_WET  "soil_wet_v"     // napięcie czujnika gleby w wodzie [V]
#define KEY_MAG_DECL  "mag_decl"       // deklinacja magnetyczna [°]
#define KEY_SOLAR_MV  "solar_mv_wm2"   // mV na W/m² - bez niego pyranometr nie startuje

#define SOIL_MAX_DRY_V 2.6f            // typowe dla czujników pojemnościowych
#define SOIL_MAX_WET_V 1.2f

static Sht4x    s_sht;
static Bmp581   s_bmp;
static Veml7700 s_veml;
static Ltr390uv s_ltr;
static Mmc5983  s_mmc;
static As5600   s_as5600;
static Ads1115  s_ads;
static PmAir    s_pm;

static Channel mkChan(const char* id, const char* name, const char* unit, int dec,
                      const char* zone, const char* haClass, const char* haUnit,
                      const char* icon) {
  Channel c;
  c.id = id;
  c.name = name;
  c.unit = unit;
  c.decimals = dec;
  c.zone = zone;
  c.haClass = haClass;
  c.haUnit = haUnit;
  c.haIcon = icon;
  return c;
}

// Czy na pinie wisi wolny przewód? Pin bez podłączonego czujnika "pływa",
// więc podciągnięcie do masy i do VCC daje skrajnie różne odczyty.
static bool analogPinFloating(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(3);
  int lo = analogRead(pin);
  pinMode(pin, INPUT_PULLUP);
  delay(3);
  int hi = analogRead(pin);
  pinMode(pin, INPUT);
  return (hi - lo) > 1500;
}

// Napięcie w woltach - średnia z kilku prób (pojedynczy odczyt ADC szumi)
static float analogVolts(int pin) {
  uint32_t mv = 0;
  for (uint8_t i = 0; i < 8; i++) {
    mv += analogReadMilliVolts(pin);
    delay(2);
  }
  return (float)(mv / 8) / 1000.0f;
}

// -------------------------------------------------------------
//  Rejestracja kanałów (wołane z begin() przed applyChannelConfig)
// -------------------------------------------------------------
void SensorManager::registerExtraChannels() {
  channels_.push_back(mkChan("sht_t", "Temperatura (SHT4x)", "°C", 1, "out",
                             "temperature", "°C", "mdi:thermometer"));
  channels_.push_back(mkChan("sht_h", "Wilgotność (SHT4x)", "%", 1, "out",
                             "humidity", "%", "mdi:water-percent"));
  channels_.push_back(mkChan("bmp_t", "Temperatura (BMP581)", "°C", 1, "in",
                             "temperature", "°C", "mdi:thermometer"));
  channels_.push_back(mkChan("bmp_p", "Ciśnienie (BMP581)", "hPa", 1, "in",
                             "pressure", "hPa", "mdi:gauge"));
  channels_.push_back(mkChan("uv", "Indeks UV", "", 1, "out",
                             "", "", "mdi:weather-sunny-alert"));
  channels_.push_back(mkChan("soil", "Wilgotność gleby", "%", 0, "out",
                             "moisture", "%", "mdi:water-outline"));
  channels_.push_back(mkChan("solar_wm2", "Nasłonecznienie", "W/m²", 0, "out",
                             "irradiance", "W/m²", "mdi:white-balance-sunny"));
  channels_.push_back(mkChan("mmc_hdg", "Azymut (kompas)", "°", 0, "out",
                             "", "°", "mdi:compass-rose"));

  // Pył i jakość powietrza wewnątrz (SEN5x albo SPS30 na I2C)
  channels_.push_back(mkChan("in_pm1", "PM 1.0 (wewnątrz)", "µg/m³", 0, "in",
                             "pm1", "µg/m³", "mdi:air-filter"));
  channels_.push_back(mkChan("in_pm25", "PM 2.5 (wewnątrz)", "µg/m³", 0, "in",
                             "pm25", "µg/m³", "mdi:air-filter"));
  channels_.push_back(mkChan("in_pm4", "PM 4.0 (wewnątrz)", "µg/m³", 0, "in",
                             "", "µg/m³", "mdi:air-filter"));
  channels_.push_back(mkChan("in_pm10", "PM 10 (wewnątrz)", "µg/m³", 0, "in",
                             "pm10", "µg/m³", "mdi:air-filter"));
  channels_.push_back(mkChan("sen_t", "Temperatura (SEN5x)", "°C", 1, "in",
                             "temperature", "°C", "mdi:thermometer"));
  channels_.push_back(mkChan("sen_h", "Wilgotność (SEN5x)", "%", 1, "in",
                             "humidity", "%", "mdi:water-percent"));
  channels_.push_back(mkChan("sen_voc", "VOC (wewnątrz)", "", 0, "in",
                             "", "", "mdi:chemical-weapon"));
  channels_.push_back(mkChan("sen_nox", "NOx (wewnątrz)", "", 0, "in",
                             "", "", "mdi:chemical-weapon"));

  // Cztery wejścia analogowe ADS1115 - domyślnie wyłączone (patrz
  // ConfigManager::channelCfg), bo zwykle wykorzystuje się tylko jedno.
  for (int i = 0; i < 4; i++) {
    Channel a = mkChan(("ads_" + String(i)).c_str(),
                       ("Wejście analogowe #" + String(i + 1) + " (ADS1115)").c_str(),
                       "V", 3, "out", "voltage", "V", "mdi:sine-wave");
    channels_.push_back(a);
  }

  // Kanały sterowników modułowych (drv_*.cpp) - każdy moduł opisuje swoje
  // kanały razem z jednostką, strefą i klasą dla Home Assistant.
  for (uint16_t m = 0; m < drvModuleCount(); m++) {
    const DrvModule* mod = drvModuleAt(m);
    if (!mod) continue;
    for (uint16_t c = 0; c < mod->chanCount; c++) {
      const ChanDef& d = mod->chans[c];
      channels_.push_back(mkChan(d.id, d.name, d.unit, d.dec, d.zone,
                                 d.haClass, d.haUnit, d.haIcon));
    }
  }
}

// -------------------------------------------------------------
//  Wykrywanie sprzętu (wołane z begin() po starcie magistrali I2C)
// -------------------------------------------------------------
bool SensorManager::beginExtra() {
  if (pinMap.pin("i2c_sda") < 0 || pinMap.pin("i2c_scl") < 0) {
    LOG_W("Magistrala I2C wyłączona w edytorze pinów - pomijam czujniki na I2C");
    drvBeginAll();   // sterowniki i tak zgłoszą brak sprzętu
    return false;
  }

  if (s_sht.begin()) {
    setPresent("sht_t", true);
    setPresent("sht_h", true);
    LOG_I("SHT4x: wykryty pod adresem 0x%02X (temperatura i wilgotność)", s_sht.addr());
  }
  if (s_bmp.begin()) {
    setPresent("bmp_t", true);
    setPresent("bmp_p", true);
    LOG_I("BMP581: wykryty pod adresem 0x%02X (ciśnienie i temperatura)", s_bmp.addr());
  }
  // VEML7700 obsługuje kanał "light" tylko wtedy, gdy nie ma BH1750
  if (s_veml.begin()) {
    if (!bhOk_) setPresent("light", true);
    LOG_I("VEML7700: wykryty (natężenie światła)");
  }
  if (s_ltr.begin()) {
    setPresent("uv", true);
    LOG_I("LTR-390UV: wykryty (indeks UV)");
  }
  if (s_mmc.begin()) {
    setPresent("mmc_hdg", true);
    LOG_I("MMC5983MA: wykryty (kompas, azymut magnetyczny)");
  }
  // AS5600 przejmuje kanał kierunku wiatru tylko wtedy, gdy pin analogowy
  // wiatrowskazu jest wyłączony w edytorze pinów.
  if (s_as5600.begin()) {
    if (pinMap.pin("vane") < 0) {
      setPresent("vane", true);
      LOG_I("AS5600: wykryty (kierunek wiatru z enkodera I2C)");
    } else {
      LOG_I("AS5600: wykryty, ale używany jest analogowy wiatrowskaz na pinie %d",
            pinMap.pin("vane"));
    }
  }
  if (s_ads.begin()) {
    LOG_I("ADS1115: wykryty pod adresem 0x%02X (4 wejścia analogowe)", s_ads.addr());
  }

  if (s_pm.begin()) {
    setPresent("in_pm1", true);
    setPresent("in_pm25", true);
    setPresent("in_pm4", true);
    setPresent("in_pm10", true);
    if (s_pm.kind() == PM_SEN5X) {
      setPresent("sen_t", true);
      setPresent("sen_h", true);
      setPresent("sen_voc", true);
      setPresent("sen_nox", true);
    }
    LOG_I("Czujnik pyłu wewnątrz: %s wykryty na I2C (0x69)", s_pm.kindName());
  }

  LOG_I("Dodatkowe czujniki: SHT4x %s, BMP581 %s, VEML7700 %s, LTR-390UV %s, MMC5983MA %s, AS5600 %s, ADS1115 %s, pył %s",
        s_sht.ok() ? "OK" : "brak", s_bmp.ok() ? "OK" : "brak",
        s_veml.ok() ? "OK" : "brak", s_ltr.ok() ? "OK" : "brak",
        s_mmc.ok() ? "OK" : "brak", s_as5600.ok() ? "OK" : "brak",
        s_ads.ok() ? "OK" : "brak",
        s_pm.kind() != PM_NONE ? s_pm.kindName() : "brak");

  // Sterowniki modułowe (drv_*.cpp): każdy sam sprawdza swoje adresy I2C
  drvBeginAll();

  return true;
}

// -------------------------------------------------------------
//  Odczyt (wołane z readAll() na końcu cyklu)
// -------------------------------------------------------------
void SensorManager::readExtra() {
  // --- SHT4x: temperatura i wilgotność na zewnątrz ---
  if (s_sht.ok()) {
    float t = NAN, h = NAN;
    if (s_sht.read(t, h)) {
      publishExtra("sht_t", t);
      publishExtra("sht_h", h);
    } else {
      clearExtra("sht_t");
      clearExtra("sht_h");
    }
  }

  // --- BMP581: ciśnienie i temperatura ---
  if (s_bmp.ok()) {
    float t = NAN, p = NAN;
    if (s_bmp.read(t, p)) {
      publishExtra("bmp_t", t);
      publishExtra("bmp_p", p);
    } else {
      clearExtra("bmp_t");
      clearExtra("bmp_p");
    }
  }

  // --- VEML7700: światło (zapas dla BH1750) ---
  if (s_veml.ok() && !bhOk_) {
    float lux = NAN;
    if (s_veml.read(lux)) publishExtra("light", lux);
  }

  // --- LTR-390UV: indeks UV ---
  if (s_ltr.ok()) {
    float uv = NAN;
    if (s_ltr.read(uv)) publishExtra("uv", uv);
  }

  // --- MMC5983MA: azymut magnetyczny (z korektą deklinacji) ---
  if (s_mmc.ok()) {
    float deg = NAN;
    if (s_mmc.readHeading(deg)) {
      deg += config.extraF(KEY_MAG_DECL, 0.0f);
      while (deg < 0.0f) deg += 360.0f;
      while (deg >= 360.0f) deg -= 360.0f;
      publishExtra("mmc_hdg", deg);
    }
  }

  // --- AS5600: kierunek wiatru z enkodera (gdy pin analogowy wyłączony) ---
  if (s_as5600.ok() && pinMap.pin("vane") < 0) {
    float deg = NAN;
    if (s_as5600.readAngle(deg)) {
      if (s_as5600.magnetOk()) publishExtra("vane", deg);
      else clearExtra("vane");   // brak magnesu = pomiar bez sensu
    }
  }

  // --- ADS1115: cztery wejścia analogowe (0..4,096 V) ---
  if (s_ads.ok()) {
    for (uint8_t i = 0; i < 4; i++) {
      float v = NAN;
      if (s_ads.readChannel(i, v)) publishExtra("ads_" + String(i), v);
    }
  }

  // --- Pyranometr na ADC (tylko gdy podano kalibrację mV na W/m²) ---
  {
    const int solPin = pinMap.pin("pyrano_adc");
    if (solPin >= 0 && config.hasExtra(KEY_SOLAR_MV)) {
      float mvPerWm2 = config.extraF(KEY_SOLAR_MV, 1.0f);
      if (mvPerWm2 > 0.01f && !analogPinFloating(solPin)) {
        float mv = analogVolts(solPin) * 1000.0f;
        float wm2 = mv / mvPerWm2;
        if (wm2 < 0.0f) wm2 = 0.0f;
        publishExtra("solar_wm2", wm2);
      }
    }
  }

  // --- Wilgotność gleby na ADC ---
  {
    const int soilPin = pinMap.pin("soil_adc");
    if (soilPin >= 0 && !analogPinFloating(soilPin)) {
      float v = analogVolts(soilPin);
      float dry = config.extraF(KEY_SOIL_DRY, SOIL_MAX_DRY_V);
      float wet = config.extraF(KEY_SOIL_WET, SOIL_MAX_WET_V);
      if (fabsf(dry - wet) < 0.05f) wet = dry - 1.0f;
      float pct = (dry - v) / (dry - wet) * 100.0f;
      pct = constrain(pct, 0.0f, 100.0f);
      publishExtra("soil", pct);
    }
  }

  // --- Pył / jakość powietrza wewnątrz (SEN5x albo SPS30) ---
  if (s_pm.kind() != PM_NONE) {
    PmData d;
    if (s_pm.read(d)) {
      if (!isnan(d.pm1))  publishExtra("in_pm1", d.pm1);
      if (!isnan(d.pm25)) publishExtra("in_pm25", d.pm25);
      if (!isnan(d.pm4))  publishExtra("in_pm4", d.pm4);
      if (!isnan(d.pm10)) publishExtra("in_pm10", d.pm10);
      if (s_pm.kind() == PM_SEN5X) {
        if (!isnan(d.tempC)) publishExtra("sen_t", d.tempC);
        if (!isnan(d.rh))    publishExtra("sen_h", d.rh);
        if (!isnan(d.voc))   publishExtra("sen_voc", d.voc);
        if (!isnan(d.nox))   publishExtra("sen_nox", d.nox);
      }
    }
  }

  // --- Sterowniki modułowe (drv_*.cpp) ---
  drvReadAll();
}

// Nazwa dodatkowego czujnika jakości powietrza do diagnostyki ("" gdy brak)
String SensorManager::extraAqName() { return String(s_pm.kindName()); }

// Zapis kanału z potwierdzeniem sprzętowym (present + detected + wartość)
void SensorManager::publishExtra(const String& id, float v) {
  ChannelConfig c = config.channelCfg(id);
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  setPresent(id, true);
  setDetected(id, true);
  putValue(id, v + c.offset);
  if (mutex_) xSemaphoreGive(mutex_);
}

// Wyczyszczenie wartości (czujnik przestał odpowiadać) bez zmiany flag
void SensorManager::clearExtra(const String& id) {
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  putValue(id, NAN, false);
  if (mutex_) xSemaphoreGive(mutex_);
}

// -------------------------------------------------------------
//  Mostek dla sterowników modułowych (drv_mod.h)
// -------------------------------------------------------------
// Sprzęt potwierdzony, ale pomiaru jeszcze nie ma (kanał czeka na pierwszą
// realną próbkę - inaczej pulpit pokazywałby zera z nieistniejącego czujnika)
void SensorManager::drvFound(const char* id) {
  if (!id || !*id) return;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  setPresent(String(id), true);
  if (mutex_) xSemaphoreGive(mutex_);
}

void SensorManager::drvPublish(const char* id, float v) {
  if (!id || !*id) return;
  publishExtra(String(id), v);
}

void SensorManager::drvClear(const char* id) {
  if (!id || !*id) return;
  clearExtra(String(id));
}

// =============================================================
//  Kanały zdalne z innych ESP (magistrala RS485)
//
//  Węzeł o adresie N przysyła listę swoich kanałów, a my odbijamy je jako
//  "xN_<id>" (np. "x2_temp", "x3_wind"). Dzięki temu kanały węzła:
//    * pokazują się na pulpicie (osobna sekcja) i w wykresach,
//    * trafiają do CSV na karcie SD oraz do Home Assistant przez MQTT,
//    * są widziane przez prognozę, pierścień LED i serwisy zewnętrzne -
//      SensorManager::valueOf() sięga po nie, gdy tej stacji brakuje
//      własnego czujnika (patrz sensors.cpp),
//    * są zapisywane razem z kanałami lokalnymi dla celów analizy pogody.
//
//  Poprawki kalibracji (offset) stosuje węzeł, dlatego wartość przepisujemy
//  1:1 - inaczej poprawka zostałaby doliczona dwa razy.
//
//  Uwaga na CSV: kolumny szerokiego pliku mają tylko kanały węzła #2 (dopisane
//  historycznie), więc kanały węzłów #3 i dalszych lądują w dzienniku długim
//  /logs/extra-YYYY-MM.csv, a wykresy czytają je stamtąd (logger.cpp).
// =============================================================

static String remoteId(uint8_t addr, const String& id) {
  return String("x") + String((unsigned)addr) + "_" + id;
}

// "x2_temp" / "x12_ds_3" => true; "xtemp" / "x_temp" / "temp" => false.
bool SensorManager::isRemoteId(const String& id) {
  if (id.length() < 3 || id[0] != 'x') return false;
  size_t i = 1;
  bool digit = false;
  while (i < id.length() && id[i] >= '0' && id[i] <= '9') { i++; digit = true; }
  return digit && i < id.length() && id[i] == '_';
}

uint8_t SensorManager::remoteAddrOf(const String& id) {
  if (!isRemoteId(id)) return 0;
  size_t i = 1;
  unsigned a = 0;
  while (i < id.length() && id[i] >= '0' && id[i] <= '9') {
    a = a * 10 + (unsigned)(id[i] - '0');
    if (a > 255) return 0;
    i++;
  }
  return (uint8_t)a;
}

int SensorManager::remoteCount(uint8_t addr) {
  int n = 0;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& c : channels_) {
    if (!c.remote) continue;
    if (addr && c.remoteAddr != addr) continue;
    n++;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return n;
}

// Usunięcie kanałów zdalnych (magistrala wyłączona, węzeł usunięty z listy
// albo węzeł przestał przysyłać swoje kanały). addr == 0 => wszystkie węzły.
void SensorManager::remoteClear(uint8_t addr) {
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (size_t i = channels_.size(); i-- > 0; ) {
    const Channel& c = channels_[i];
    if (!c.remote) continue;
    if (addr && c.remoteAddr != addr) continue;
    channels_.erase(channels_.begin() + i);
  }
  if (mutex_) xSemaphoreGive(mutex_);
}

// Wołane przed każdym odczytem węzła: brak odpowiedzi nie kasuje kanałów
// (nie znikają z pulpitu przy chwilowym zerwaniu), ale nie pokazują już
// starych wartości. addr == 0 => wszystkie węzły.
void SensorManager::remoteBegin(uint8_t addr) {
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  for (auto& c : channels_) {
    if (!c.remote) continue;
    if (addr && c.remoteAddr != addr) continue;
    c.value = NAN;
    c.detected = false;
    c.measured = false;
  }
  if (mutex_) xSemaphoreGive(mutex_);
}

// Zapis jednego kanału odebranego z węzła (tworzy kanał przy pierwszym razie).
void SensorManager::remotePublish(uint8_t addr, const String& id, const String& name,
                                  const String& unit, const String& zone, float v,
                                  bool detected, bool measured, int decimals,
                                  const String& haClass, const String& haUnit,
                                  const String& haIcon) {
  if (!addr || id.length() == 0) return;
  String rid = remoteId(addr, id);
  ChannelConfig cc = config.channelCfg(rid);   // lokalne nadpisania (nazwa/strefa/włączenie)

  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  Channel* c = ch(rid);
  if (!c) {
    Channel n;
    n.id = rid;
    n.remote = true;
    n.remoteAddr = addr;
    channels_.push_back(n);
    c = &channels_.back();
  }
  c->remote = true;
  c->remoteAddr = addr;
  c->unit = unit;
  c->decimals = decimals;
  c->haClass = haClass;
  c->haUnit = haUnit;
  c->haIcon = haIcon;
  // Nazwa kanału z numerem węzła pomaga odróżnić źródła na jednym pulpicie;
  // użytkownik może ją zmienić w zakładce Kalibracja.
  c->name = name.length() ? name + " (ESP #" + String((unsigned)addr) + ")" : rid;
  c->zone = (zone == "in") ? "in" : "out";
  c->enabled = cc.enabled;
  if (cc.name.length()) c->name = cc.name;
  if (cc.zone == "in" || cc.zone == "out") c->zone = cc.zone;
  c->present = true;
  c->detected = detected;
  c->value = measured ? v : NAN;
  c->measured = measured;
  if (mutex_) xSemaphoreGive(mutex_);
}

// Odświeżenie wartości już znanego kanału zdalnego - używane przy odpytywaniu
// samych pomiarów ("vals"), gdzie metadane kanału przychodzą tylko raz.
bool SensorManager::remoteValue(uint8_t addr, const String& id, float v, bool detected,
                                bool measured) {
  if (!addr || !id.length()) return false;
  String rid = remoteId(addr, id);
  bool found = false;
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  Channel* c = ch(rid);
  if (c && c->remote) {
    c->remoteAddr = addr;
    c->present = true;
    c->detected = detected;
    c->value = measured ? v : NAN;
    c->measured = measured;
    found = true;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return found;
}
