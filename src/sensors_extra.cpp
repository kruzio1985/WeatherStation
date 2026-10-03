/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
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
#include <SoftwareSerial.h>
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
#define KEY_UV_MV     "uv_mv_uvi"      // mV na 1 UVI (GUVA-S12SD: 100 mV/UVI)

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

// -------------------------------------------------------------
//  Import ze stacji jakości powietrza (urządzenie zewnętrzne typu "aq")
//
//  Pozycje stacji powietrza (identyfikatory z jej /api/status) mapujemy na
//  własne kanały strefy "wewnątrz". To jedyne miejsce, w którym trzeba coś
//  dopisać, gdy stacja powietrza doda kolejny czujnik.
//
//  scale - mnożnik jednostek (stacja podaje TVOC w ppm, nasz kanał jest w ppb)
//  owner - własny czujnik tej stacji, który ma pierwszeństwo nad importem:
//          'p' pyłomierz (PMS/SEN5x), 'c' SCD4x, 'v' SGP30/SGP40; 0 = zawsze
//  name  - nullptr = kanał jest już zarejestrowany wyżej, tylko importujemy
// -------------------------------------------------------------
struct AirImportRow {
  const char* src;
  const char* chan;
  float       scale;
  char        owner;
  const char* name;      // nullptr = nie rejestrujemy (kanał już istnieje)
  const char* unit;
  uint8_t     dec;
  const char* haClass;
  const char* haUnit;
  const char* haIcon;
};

static const AirImportRow AIR_IMPORT[] = {
  // --- kanały już zarejestrowane w tej stacji (tylko uzupełniamy odczytem) ---
  {"pm1",     "in_pm1",       1.0f, 'p'},
  {"pm25",    "in_pm25",      1.0f, 'p'},
  {"pm10",    "in_pm10",      1.0f, 'p'},
  {"co2",     "co2",          1.0f, 'c'},
  {"tvoc",    "tvoc",      1000.0f, 'v'},   // stacja podaje ppm, kanał jest w ppb
  {"ch2o",    "ch2o",         1.0f,  0 },
  // --- cząsteczki: średnie kroczące stacji powietrza ---
  {"pm25_1m",  "in_pm25_1m",  1.0f, 'p', "PM 2.5 · 1 min (wewnątrz)", "µg/m³", 0,
               "pm25", "µg/m³", "mdi:air-filter"},
  {"pm25_5m",  "in_pm25_5m",  1.0f, 'p', "PM 2.5 · 5 min (wewnątrz)", "µg/m³", 0,
               "pm25", "µg/m³", "mdi:air-filter"},
  {"pm25_24h", "in_pm25_24h", 1.0f, 'p', "PM 2.5 · 24 h (wewnątrz)", "µg/m³", 0,
               "pm25", "µg/m³", "mdi:air-filter"},
  // --- indeks jakości powietrza liczony przez stację powietrza ---
  {"aqi",     "in_iaq",       1.0f,  0,  "IAQ (stacja powietrza)", "", 0,
               "aqi", "", "mdi:air-filter"},
  {"tvoclvl", "in_tvoc_lvl",  1.0f,  0,  "Poziom TVOC (wewnątrz)", "", 0,
               "", "", "mdi:chemical-weapon"},
  // --- gazy: MiCS5524 i MQ-7 (przez ADS1115 na stacji powietrza) ---
  {"mics_v",  "in_mics_v",    1.0f,  0,  "Napięcie (MiCS5524)", "V", 3,
               "voltage", "V", "mdi:sine-wave"},
  {"mics_rs", "in_mics_rs",   1.0f,  0,  "Rs/R0 (MiCS5524)", "", 2,
               "", "", "mdi:scale-balance"},
  {"mics_co", "in_mics_co",   1.0f,  0,  "CO ekwiwalent (MiCS5524)", "ppm", 1,
               "carbon_monoxide", "ppm", "mdi:molecule-co"},
  {"mics_wu", "in_mics_wu",   1.0f,  0,  "Wygrzewanie (MiCS5524)", "", 0,
               "", "", "mdi:timer-sand"},
  {"mq7_v",   "in_mq7_v",     1.0f,  0,  "Napięcie (MQ-7)", "V", 3,
               "voltage", "V", "mdi:sine-wave"},
  {"mq7_rs",  "in_mq7_rs",    1.0f,  0,  "Rs/R0 (MQ-7)", "", 2,
               "", "", "mdi:scale-balance"},
  {"mq7_co",  "in_mq7_co",    1.0f,  0,  "CO (MQ-7)", "ppm", 1,
               "carbon_monoxide", "ppm", "mdi:molecule-co"},
  // --- temperatura, wilgotność i ruch mierzone przez stację powietrza ---
  {"temp",    "in_air_t",     1.0f,  0,  "Temperatura (stacja powietrza)", "°C", 1,
               "temperature", "°C", "mdi:thermometer"},
  {"hum",     "in_air_h",     1.0f,  0,  "Wilgotność (stacja powietrza)", "%", 1,
               "humidity", "%", "mdi:water-percent"},
  {"temp_24h","in_air_t24",   1.0f,  0,  "Temperatura · 24 h (stacja powietrza)", "°C", 1,
               "temperature", "°C", "mdi:thermometer"},
  {"hum_24h", "in_air_h24",   1.0f,  0,  "Wilgotność · 24 h (stacja powietrza)", "%", 1,
               "humidity", "%", "mdi:water-percent"},
  {"tempext", "in_air_t_ext", 1.0f,  0,  "Temperatura zewn. (stacja powietrza)", "°C", 1,
               "temperature", "°C", "mdi:thermometer"},
  {"humext",  "in_air_h_ext", 1.0f,  0,  "Wilgotność zewn. (stacja powietrza)", "%", 1,
               "humidity", "%", "mdi:water-percent"},
  {"rhcorr",  "in_rh_corr",   1.0f,  0,  "Korekcja RH (stacja powietrza)", "%", 1,
               "humidity", "%", "mdi:water-percent"},
  {"motion",  "in_motion",    1.0f,  0,  "Ruch (stacja powietrza)", "", 0,
               "", "", "mdi:motion-sensor"},
};

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

  // Czujnik Tuya (TLSR8258, firmware UART) zasila główne kanały "temp"/"hum"
  // (rejestrowane w SensorManager::begin) - osobne kanały tuy_t/tuy_h nie są
  // potrzebne, żeby nie dublować odczytów na pulpicie i w Home Assistant.
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

  // Formaldehyd - na tej stacji nie ma własnego czujnika, więc kanał wypełnia
  // zewnętrzna stacja jakości powietrza (patrz ingestAirStation).
  channels_.push_back(mkChan("ch2o", "Formaldehyd (wewnątrz)", "ppb", 0, "in",
                             "volatile_organic_compounds_parts", "ppb",
                             "mdi:chemical-weapon"));

  // Pozostałe kanały przejmowane ze stacji jakości powietrza: średnie PM,
  // indeks IAQ, gazy MiCS5524 i MQ-7 oraz temperatura/wilgotność/ruch z tej
  // stacji. Definicje siedzą w tabeli AIR_IMPORT - dopisanie kolejnego
  // czujnika to jedna linijka tam, bez zmian w tym miejscu.
  for (const auto& row : AIR_IMPORT) {
    if (!row.name) continue;
    channels_.push_back(mkChan(row.chan, row.name, row.unit, row.dec, "in",
                               row.haClass ? row.haClass : "",
                               row.haUnit ? row.haUnit : "",
                               row.haIcon ? row.haIcon : ""));
  }

  // Analiza zdjęcia z kamery (camera.cpp, publishAnalysis). Kanały powstają
  // dopiero po pierwszym zdjęciu - do tego czasu są niewidoczne (bez
  // potwierdzonego odczytu nie ma ich na pulpicie ani w Home Assistant).
  // Kod pogody: 0 = noc, 1 = bezchmurnie, 2 = pochmurno, 3 = deszcz,
  // 4 = śnieg, 5 = mgła. Faza dnia: 0 = noc, 1 = świt/zmierzch, 2 = dzień.
  channels_.push_back(mkChan("cam_weather", "Pogoda z kamery", "", 0, "out",
                             "", "", "mdi:image-filter-drama"));
  channels_.push_back(mkChan("cam_phase", "Faza dnia (kamera)", "", 0, "out",
                             "", "", "mdi:theme-light-dark"));
  channels_.push_back(mkChan("cam_cloud", "Zachmurzenie (kamera)", "%", 0, "out",
                             "", "%", "mdi:cloud"));
  channels_.push_back(mkChan("cam_rain", "Deszcz (kamera)", "%", 0, "out",
                             "precipitation_intensity", "%", "mdi:weather-rainy"));
  channels_.push_back(mkChan("cam_snow", "Śnieg (kamera)", "%", 0, "out",
                             "precipitation_intensity", "%", "mdi:weather-snowy"));
  channels_.push_back(mkChan("cam_fog", "Mgła (kamera)", "%", 0, "out",
                             "", "%", "mdi:weather-fog"));
  channels_.push_back(mkChan("cam_lux", "Światło (kamera)", "lx", 0, "out",
                             "illuminance", "lx", "mdi:brightness-6"));

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
  beginTuya();   // UART działa niezależnie od magistrali I2C

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
  } else if (pinMap.pin("uv_adc") >= 0) {
    // Analogowy czujnik UV (GUVA-S12SD i podobne): napięcie 0-1 V -> UVI
    setPresent("uv", true);
    LOG_I("UV analogowy: pin ADC %d (GUVA-S12SD, %d mV/UVI)",
          pinMap.pin("uv_adc"), (int)config.extraF(KEY_UV_MV, 100.0f));
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

  // --- Analogowy czujnik UV (GUVA-S12SD): UVI = mV / kalibracja ---
  if (!s_ltr.ok()) {
    const int uvPin = pinMap.pin("uv_adc");
    if (uvPin >= 0 && !analogPinFloating(uvPin)) {
      float mvPerUvi = config.extraF(KEY_UV_MV, 100.0f);
      if (mvPerUvi < 1.0f) mvPerUvi = 100.0f;
      float mv = analogVolts(uvPin) * 1000.0f;
      float uvi = mv / mvPerUvi;
      if (uvi < 0.0f) uvi = 0.0f;

      // Diagnostyka: surowy odczyt ADC pozwala odróżnić przesterowanie
      // przetwornika (raw 4095) od realnego napięcia z modułu.
      static unsigned long uvDiagMs = 0;
      if (millis() - uvDiagMs > 300000UL) {
        uvDiagMs = millis();
        int raw = analogRead(uvPin);
        LOG_I("UV analogowy: raw=%d/4095, %.0f mV, kalibracja %.0f mV/UVI",
              raw, (double)mv, (double)mvPerUvi);
      }
      publishExtra("uv", uvi);
    }
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

// -------------------------------------------------------------
//  Czujnik Tuya temp./wilg. (TLSR8258 + CHT8305, firmware UART)
//
//  Czujnik wysyła linię ASCII "T=xx.xx;RH=yy.yy" co ok. 2 s po UART
//  115200 8N1. Wszystkie 3 sprzętowe UART-y na S3 są zajęte (RS485 / GPS /
//  PMS5003), więc odbiór idzie po SoftwareSerial na pinie "tuy_rx".
//  Parsowanie jest celowo tolerancyjne: akceptuje \r\n i \n, spacje oraz
//  opcjonalny średnik na końcu (niektóre wersje firmware go dokładają).
// -------------------------------------------------------------
static bool parseTuyaLine(const String& line, float& t, float& h) {
  int ti = line.indexOf("T=");
  int hi = line.indexOf("RH=");
  if (ti < 0 || hi < 0) return false;

  int tEnd = line.indexOf(';', ti);
  String ts = line.substring(ti + 2, tEnd < 0 ? line.length() : tEnd);
  String hs = line.substring(hi + 3);
  ts.trim();
  hs.trim();
  if (hs.endsWith(";")) hs = hs.substring(0, hs.length() - 1);
  hs.trim();
  if (ts.length() == 0 || hs.length() == 0) return false;

  float tv = ts.toFloat();
  float hv = hs.toFloat();
  if (!isfinite(tv) || !isfinite(hv)) return false;
  // Wartości spoza fizycznego zakresu = uszkodzona linia, nie pomiar
  if (tv < -55.0f || tv > 85.0f) return false;
  if (hv < 0.0f || hv > 100.0f) return false;

  t = tv;
  h = hv;
  return true;
}

void SensorManager::beginTuya() {
#if STACJA_ROLE_MASTER
  const int rx = pinMap.pin("tuy_rx");
  if (rx < 0) {
    LOG_I("Tuya UART: wyłączony (pin RX = -1)");
    return;
  }

  // Na ESP32-S3 sprzętowy UART2 jest wolny (PMS5003 niepodłączony) - jest
  // znacznie pewniejszy niż SoftwareSerial przy 115200 z włączonym Wi-Fi.
#if SOC_UART_NUM > 2
  Serial2.begin(115200, SERIAL_8N1, rx, -1);
  tuyHwUart_ = true;
  tuyStarted_ = true;
#else
  SoftwareSerial* ss = new SoftwareSerial(rx, -1, false);
  ss->begin(115200);
  tuySerial_ = ss;
  tuyStarted_ = true;
#endif
  tuyLastRxMs_ = millis();

  setPresent("temp", true);
  setPresent("hum", true);
  LOG_I("Tuya UART: RX na GPIO %d, 115200 8N1 (%s)", rx,
        tuyHwUart_ ? "UART2 sprzętowy" : "SoftwareSerial");
#else
  // Na węźle głównym czujnikiem temp./wilg. jest BME280; Tuya jest tylko na
  // masterze, więc niczego nie nadpisujemy i nie oznaczamy kanałów temp/hum.
  LOG_I("Tuya UART: rola węzeł - pomijam (temp./wilg. podaje BME280)");
#endif
}

void SensorManager::serviceTuya() {
  if (!tuyStarted_) return;

  // Jedno wspólne czytanie dla UART2 sprzętowego i SoftwareSerial.
  int c;
#if SOC_UART_NUM > 2
  if (tuyHwUart_) {
    while (Serial2.available()) {
      c = Serial2.read();
      tuyRawBytes_++;
      tuyLastByte_ = c;
      tuyLastRxMs_ = millis();
      if (c == '\n') {
        String line = tuyBuf_;
        tuyBuf_ = "";
        line.trim();
        if (line.length()) tuyLastLine_ = line;   // diagnostyka: co naprawdę przyszło
        float t = NAN, h = NAN;
        if (parseTuyaLine(line, t, h)) {
          publishExtra("temp", t);
          publishExtra("hum", h);
          if (!tuyOk_) {
            tuyOk_ = true;
            LOG_I("Tuya UART: odebrano poprawną linię - czujnik działa");
          }
        }
      } else if (c != '\r') {
        tuyBuf_ += (char)c;
        if (tuyBuf_.length() > 64) tuyBuf_ = tuyBuf_.substring(tuyBuf_.length() - 64);
      }
    }
  } else
#endif
  {
    SoftwareSerial* ss = (SoftwareSerial*)tuySerial_;
    if (!ss) return;
    while (ss->available()) {
      c = ss->read();
      tuyRawBytes_++;
      tuyLastByte_ = c;
      tuyLastRxMs_ = millis();
      if (c == '\n') {
        String line = tuyBuf_;
        tuyBuf_ = "";
        line.trim();
        if (line.length()) tuyLastLine_ = line;   // diagnostyka: co naprawdę przyszło
        float t = NAN, h = NAN;
        if (parseTuyaLine(line, t, h)) {
          publishExtra("temp", t);
          publishExtra("hum", h);
          if (!tuyOk_) {
            tuyOk_ = true;
            LOG_I("Tuya UART: odebrano poprawną linię - czujnik działa");
          }
        }
      } else if (c != '\r') {
        tuyBuf_ += (char)c;
        if (tuyBuf_.length() > 64) tuyBuf_ = tuyBuf_.substring(tuyBuf_.length() - 64);
      }
    }
  }

  // Diagnostyka: sygnał na pinie jest (licznik rośnie), ale linia nie pasuje
  // do "T=xx.xx;RH=yy.yy" - to zwykle zła prędkość UART, złe podłączenie albo
  // inny nadajnik na tej linii. W logu pokazujemy ostatnią pełną linię, bo
  // sam licznik bajtów nie mówi, co właściwie przychodzi.
  if (!tuyOk_ && tuyRawBytes_ > 0 && millis() - tuyLastDiagMs_ > 15000UL) {
    tuyLastDiagMs_ = millis();
    LOG_W("Tuya UART: odebrano %lu bajtów (ostatni 0x%02X), brak poprawnej linii (ostatnia: '%s')",
          (unsigned long)tuyRawBytes_, (unsigned)(tuyLastByte_ & 0xFF), tuyLastLine_.c_str());
  }

  // Czujnik odłączony / przestał nadawać - nie pokazuj nieaktualnych wartości
  if (tuyOk_ && millis() - tuyLastRxMs_ > 60000UL) {
    tuyOk_ = false;
    clearExtra("temp");
    clearExtra("hum");
    LOG_W("Tuya UART: brak danych od 60 s - czujnik odłączony?");
  }
}

// Nazwa dodatkowego czujnika jakości powietrza do diagnostyki ("" gdy brak)
String SensorManager::extraAqName() { return String(s_pm.kindName()); }

// -------------------------------------------------------------
//  Kompensacja nasłonecznienia czujników temperatury na płytce
//
//  Czujniki na płytce (BMP280, BME280, BMP581, SHT4x...) siedzą w tej samej
//  obudowie co ESP32 i pierścień LED, więc w słońcu mierzą temperaturę
//  obudowy, a nie powietrza - potrafi być o kilkanaście, a nawet o 30 °C
//  wyższa. Nadwyżkę liczymy z natężenia światła (nasłonecznienie), a wiatr
//  ją zmniejsza, bo przewiew chłodzi obudowę:
//
//      nadwyzka = btc_sun * sqrt(lux / 1000) - btc_wind * wiatr [m/s]
//      T_poprawiona = T_pomiar - nadwyzka
//
//  Pierwiastek, a nie proporcja wprost, bo nagrzewanie obudowy nasyca się
//  przy pełnym słońcu (rośnie szybciej na starcie dnia niż w południe).
//
//  Część stałą błędu (ciepło własne elektroniki, brak przewiewu nocą)
//  koryguje zwykły offset kanału w zakładce Kalibracja - oba mechanizmy
//  się uzupełniają, a nie dublują.
//
//  Ustawienia (config.extra): btc_on (1/0), btc_sun [°C na √klx] wspólny dla
//  wszystkich kanałów, btc_wind [°C na m/s] oraz btc_ids - lista kanałów po
//  przecinku. Przy identyfikatorze można podać własny współczynnik po
//  dwukropku ("ths_bmp280_t:4.3,temp:2.75"), bo czujniki w tej samej
//  obudowie grzeją się różnie.
// -------------------------------------------------------------
#define KEY_BTC_ON    "btc_on"
#define KEY_BTC_SUN   "btc_sun"
#define KEY_BTC_WIND  "btc_wind"
#define KEY_BTC_IDS   "btc_ids"
#define KEY_BTC_POW   "btc_pow"

#define BTC_DEFAULT_IDS  "ths_bmp280_t"
#define BTC_DEFAULT_SUN  0.97f    // dobrane na tej stacji z pomiarów (BMP280 w obudowie)
#define BTC_DEFAULT_WIND 1.5f
#define BTC_DEFAULT_POW  0.65f    // zmierzony kształt: nadwyżka ~ klx^0,65

// Domyślnie korekta jest wyłączona - korekta zależy od obudowy konkretnej
// stacji, więc włącza się ją świadomie (zakładka Kalibracja, przełącznik
// "Kompensacja włączona"). Pozostałe węzły magistrali nie zmienią przez
// przypadek swoich odczytów.
#define BTC_DEFAULT_ON   0.0f

// Współczynnik i wykładnik nasłonecznienia dla kanału. Na liście można podać
// sam identyfikator (wtedy obowiązują wspólne ustawienia), identyfikator
// z własnym współczynnikiem ("temp:0.97") albo z współczynnikiem
// i wykładnikiem ("ths_bmp280_t:0.333:1.0"). Oba parametry są potrzebne:
// zmierzone nadwyżki grzejącego się układu rosną z nasłonecznieniem w innej
// potędze niż nadwyżka powietrza w obudowie (czujnik na płytce ma własne
// ciepło, które rośnie prawie liniowo z promieniowaniem).
// sun = NAN oznacza, że kanału nie ma na liście.
struct BtcCoeff {
  float sun = NAN;
  float pw  = 0.65f;
};

static BtcCoeff btcCoeffFor(const String& list, const String& id,
                            float globalSun, float globalPw) {
  BtcCoeff out;
  out.pw = globalPw;
  const int len = (int)list.length();
  int start = 0;
  while (start < len) {
    int comma = list.indexOf(',', start);
    String token = comma < 0 ? list.substring(start) : list.substring(start, comma);
    token.trim();
    if (token.length()) {
      const int c1 = token.indexOf(':');
      String one = c1 < 0 ? token : token.substring(0, c1);
      one.trim();
      if (one.equalsIgnoreCase(id)) {
        if (c1 < 0) { out.sun = globalSun; return out; }
        const String rest = token.substring(c1 + 1);
        const int c2 = rest.indexOf(':');
        const float own = (c2 < 0 ? rest : rest.substring(0, c2)).toFloat();
        out.sun = own > 0.0f ? own : globalSun;
        if (c2 >= 0) {
          const float pw = rest.substring(c2 + 1).toFloat();
          if (pw > 0.05f && pw < 3.0f) out.pw = pw;
        }
        return out;
      }
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  return out;
}

// Wiatr do kompensacji w m/s. Kanały trzymają km/h, a na tej stacji wiatr
// mierzy tylko VEVOR - własny anemometr jest zapasem, gdyby był podłączony.
float SensorManager::boardWindMs() {
  float w = valueOf("wind");
  if (isnan(w)) w = valueOf("vev_wind");
  return isnan(w) ? NAN : w / 3.6f;
}

float SensorManager::sunCompensate(const String& id, float v) {
  if (isnan(v)) return v;
  if (config.extraF(KEY_BTC_ON, BTC_DEFAULT_ON) < 0.5f) return v;

  const BtcCoeff bc = btcCoeffFor(config.extraS(KEY_BTC_IDS, BTC_DEFAULT_IDS), id,
                                  config.extraF(KEY_BTC_SUN, BTC_DEFAULT_SUN),
                                  config.extraF(KEY_BTC_POW, BTC_DEFAULT_POW));
  if (isnan(bc.sun) || bc.sun <= 0.0f) return v;   // kanał nie jest kompensowany

  // Nasłonecznienie: czujnik na płytce (BH1750) nasyca się w pełnym słońcu na
  // 54,6 klx, więc w środku dnia przestaje cokolwiek mówić o słońcu i korekta
  // "nie widzi", że po południu promieniowanie spada. Czujnik stacji VEVOR ma
  // szerszy zakres, dlatego jego używamy, gdy jest dostępny.
  float lux = valueOf("vev_light");
  if (isnan(lux) || lux <= 0.0f) lux = valueOf("light");
  if (isnan(lux) || lux <= 0.0f) return v;      // nocą nie ma czego korygować

  float excess = bc.sun * powf(lux / 1000.0f, bc.pw);
  const float windK = config.extraF(KEY_BTC_WIND, BTC_DEFAULT_WIND);
  if (windK > 0.0f) {
    const float wind = boardWindMs();
    if (!isnan(wind)) excess -= windK * wind;
  }
  if (excess <= 0.0f) return v;

  static unsigned long lastLog = 0;
  const unsigned long now = millis();
  if (now - lastLog >= 60000UL) {
    lastLog = now;
    LOG_I("Kompensacja słońca: %s %.1f -> %.1f °C (%.1f klx, nadwyżka %.1f °C)",
          id.c_str(), (double)v, (double)(v - excess), (double)(lux / 1000.0f),
          (double)excess);
  }
  return v - excess;
}

// -------------------------------------------------------------
//  Stacja jakości powietrza (osobne ESP32, odpytywane po HTTP)
//
//  Dane wchodzą do kanałów WEWNĘTRZNYCH, ale tylko tam, gdzie ta stacja nie
//  ma własnego czujnika. Gdy na płytce pojawi się SEN5x albo SCD40, jego
//  odczyt ma pierwszeństwo, a import sam się wyłączy dla tego kanału.
//
//  Jednostki: stacja powietrza podaje TVOC w ppm, a kanał "tvoc" tej stacji
//  jest w ppb (tak jak SGP30), więc przeliczamy (1 ppm = 1000 ppb).
// -------------------------------------------------------------
void SensorManager::ingestAirStation(const AirStationData& d) {
  const bool localPm   = (s_pm.kind() != PM_NONE);
  const bool localCo2  = scdOk_;
  const bool localTvoc = sgpOk_;

  // Import tabelaryczny: jeden przebieg po AIR_IMPORT załatwia wszystkie
  // kanały (PM i średnie, IAQ, gazy MiCS/MQ-7, temperatura, wilgotność, ruch).
  // Kanał, dla którego ta stacja ma własny czujnik, nie jest nadpisywany.
  int imported = 0;
  for (const auto& row : AIR_IMPORT) {
    const float v = d.get(row.src);
    if (isnan(v)) continue;
    const bool haveLocal = (row.owner == 'p') ? localPm
                         : (row.owner == 'c') ? localCo2
                         : (row.owner == 'v') ? localTvoc
                                              : false;
    if (haveLocal) continue;
    publishExtra(row.chan, v * row.scale);
    imported++;
  }

  static unsigned long lastLog = 0;
  if (millis() - lastLog >= 300000UL) {
    lastLog = millis();
    LOG_I("Stacja powietrza: %d odczytów w pomiarach wewnętrznych "
          "(PM1 %.0f, PM2.5 %.0f, PM10 %.0f, CO2 %.0f ppm, TVOC %.0f ppb, "
          "IAQ %.0f, CO MiCS %.1f ppm)",
          imported,
          (double)d.get("pm1"), (double)d.get("pm25"), (double)d.get("pm10"),
          (double)d.get("co2"),
          (double)(isnan(d.get("tvoc")) ? NAN : d.get("tvoc") * 1000.0f),
          (double)d.get("aqi"), (double)d.get("mics_co"));
  }
}

// Zapis kanału z potwierdzeniem sprzętowym (present + detected + wartość)
void SensorManager::publishExtra(const String& id, float v) {
  ChannelConfig c = config.channelCfg(id);
  // Korekta przed zajęciem mutexu - sunCompensate() czyta inne kanały (light,
  // wind), a valueOf() sam bierze ten sam mutex.
  v = sunCompensate(id, v);

  // Główna wilgotność z czujnika zewnętrznego: czujnik w ogrzanej obudowie
  // zapisuje tu swoje zaniżone RH co ~2 s (serviceTuya), więc podmiana robiona
  // raz na 5 s w bridgeMissing() nie utrzymałaby się. Podstawiamy przy samym
  // zapisie, żeby wartość VEVOR była tą obowiązującą.
  if (id == "hum" && config.extraF(KEY_HUM_FROM_VEV, 0.0f) >= 0.5f) {
    const float ext = valueOf("vev_hum");
    if (!isnan(ext)) v = ext;
  }

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
//  Bramka BLE (ESP32-C3) - kanały btgw_<mac>_*
//
//  Każdy czujnik BT dostaje własny zestaw kanałów:
//    btgw_<mac>_t     temperatura  (°C)
//    btgw_<mac>_h     wilgotność  (%)
//    btgw_<mac>_bat   bateria     (%)
//    btgw_<mac>_rssi  sygnał      (dBm)
//  Kanały są zwykłe (nie "remote"), więc pokazują się na pulpicie w strefie
//  i trafiają do CSV/MQTT/Home Assistant jak każdy inny czujnik.
// =============================================================
void SensorManager::ingestBleSensor(const String& mac, const String& name,
                                    float temp, float hum, int batt, int rssi) {
  if (!mac.length()) return;
  const String base = "btgw_" + mac;
  const String label = name.length() ? name : String("BT ") + mac;

  auto pub = [&](const char* suffix, const char* nm, const char* unit, uint8_t dec,
                 const char* haClass, const char* haUnit, const char* icon,
                 float v, bool have) {
    const String id = base + suffix;
    const ChannelConfig cc = config.channelCfg(id);
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    Channel* c = ch(id);
    if (!c) {
      Channel n; n.id = id;
      channels_.push_back(n);
      c = &channels_.back();
    }
    c->name = label + " " + nm;
    c->unit = unit;
    c->decimals = dec;
    c->zone = "in";
    c->haClass = haClass; c->haUnit = haUnit; c->haIcon = icon;
    c->enabled = cc.enabled;
    if (cc.name.length()) c->name = cc.name;
    if (cc.zone == "in" || cc.zone == "out") c->zone = cc.zone;
    c->present = true;
    if (have) { c->detected = true; c->measured = true; c->value = v; }
    if (mutex_) xSemaphoreGive(mutex_);
  };

  pub("_t",    "temperatura", "°C", 1, "temperature", "°C", "mdi:thermometer",
      temp, !isnan(temp));
  pub("_h",    "wilgotność",  "%",  1, "humidity",    "%",  "mdi:water-percent",
      hum, !isnan(hum));
  pub("_bat",  "bateria",     "%",  0, "battery",     "%",  "mdi:battery",
      (float)batt, batt >= 0);
  pub("_rssi", "sygnał",      "dBm", 0, "signal_strength", "dBm", "mdi:signal",
      (float)rssi, true);
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
