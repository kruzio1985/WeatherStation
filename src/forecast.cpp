/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "forecast.h"
#include "astro.h"
#include "sensors.h"
#include "syslog.h"
#include <math.h>
#include <string.h>

ForecastService forecast;

// =============================================================
//  Prognoza lokalna - implementacja (opis w forecast.h).
//  Wszystko liczone lokalnie z danych czujników, bez chmury.
// =============================================================

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline bool  ok(float v) { return !isnan(v) && !isinf(v); }

// ------------------------------------------------------------
//  Wybór źródeł danych: najpierw czujniki precyzyjne, potem
//  BME280, a na końcu dowolny kanał zewnętrzny tego samego typu.
// ------------------------------------------------------------
static float pickValue(const char* const* ids, size_t n, const char* haClassFallback) {
  for (size_t i = 0; i < n; i++) {
    float v = sensors.valueOf(ids[i]);
    if (ok(v)) return v;
  }
  if (!haClassFallback) return NAN;

  // Dawniej szła tu kopia całej listy kanałów; na ESP32-C3 bez PSRAM kończyło
  // się to std::bad_alloc i restartem stacji w pętli (setup() -> pushSample()).
  return sensors.valueByHaClass(haClassFallback, "out");
}

static const char* const T_IDS[] = {"sht_t", "bmp_t", "temp"};
static const char* const H_IDS[] = {"sht_h", "hum"};
static const char* const P_IDS[] = {"bmp_p", "press"};

// ------------------------------------------------------------
//  Wskaźniki meteorologiczne
// ------------------------------------------------------------
static float dewPoint(float t, float rh) {
  if (!ok(t) || !ok(rh)) return NAN;
  double a = 17.62, b = 243.12;
  double g = log(fmax((double)rh, 1.0) / 100.0) + (a * t) / (b + t);
  return (float)((b * g) / (a - g));
}

static float absoluteHumidity(float t, float rh) {
  if (!ok(t) || !ok(rh)) return NAN;
  double es = 6.112 * exp((17.62 * t) / (243.12 + t));   // hPa
  return (float)(216.7 * (rh / 100.0 * es) / (t + 273.15));
}

static float heatIndex(float t, float rh) {
  if (!ok(t) || !ok(rh) || t < 26.0f) return NAN;
  double T = t * 9.0 / 5.0 + 32.0;
  double R = rh;
  double hi = -42.379 + 2.04901523 * T + 10.14333127 * R - 0.22475541 * T * R
              - 0.00683783 * T * T - 0.05481717 * R * R + 0.00122874 * T * T * R
              + 0.00085282 * T * R * R - 0.00000199 * T * T * R * R;
  return (float)((hi - 32.0) * 5.0 / 9.0);
}

static float windChill(float t, float kmh) {
  if (!ok(t) || !ok(kmh) || t > 10.0f || kmh < 4.8f) return NAN;
  double v = pow((double)kmh, 0.16);
  return (float)(13.12 + 0.6215 * t - 11.37 * v + 0.3965 * t * v);
}

static float humidex(float t, float rh) {
  if (!ok(t) || !ok(rh)) return NAN;
  double e = 6.112 * exp((17.62 * t) / (243.12 + t)) * (rh / 100.0);
  return (float)(t + 0.5555 * (e - 10.0));
}

// ------------------------------------------------------------
//  Cykliczne życie serwisu
// ------------------------------------------------------------
void ForecastService::begin() {
  for (size_t i = 0; i < FC_HIST_MAX; i++) hist_[i] = Sample();
  for (size_t i = 0; i < 32; i++) strikes_[i] = Strike();
  histCount_ = histHead_ = 0;
  strikeCount_ = strikeHead_ = 0;
  lastSampleMs_ = 0;
  started_ = false;
  info_ = ForecastInfo();
  LOG_I("Prognoza: historia %u h (probka co %u s)",
        (unsigned)(FC_HIST_MAX * (FC_SAMPLE_MS / 1000UL) / 3600UL),
        (unsigned)(FC_SAMPLE_MS / 1000UL));
}

void ForecastService::loop() {
  unsigned long now = millis();
  if (!started_) {
    started_ = true;
    lastSampleMs_ = now;
    pushSample();
    return;
  }
  if (now - lastSampleMs_ >= FC_SAMPLE_MS) {
    lastSampleMs_ = now;
    pushSample();
  }
}

void ForecastService::pushSample() {
  const time_t epoch = time(nullptr);
  const bool clockOk = epoch >= 1600000000;
  const time_t last = histCount_ ? sampleAt(histCount_ - 1)->t : 0;

  // Po udanej synchronizacji NTP zegar potrafi przeskoczyć o lata - próbki
  // zebrane wcześniej mają błędne znaczniki i zaciemniałyby wykres historii.
  if (clockOk && (!last || epoch - last > 3600 || last - epoch > 3600)) {
    histCount_ = histHead_ = 0;
  }

  Sample s;
  s.ms = millis();
  s.t  = clockOk ? epoch : 0;
  s.temp  = pickValue(T_IDS, 3, "temperature");
  s.hum   = pickValue(H_IDS, 2, "humidity");
  s.press = pickValue(P_IDS, 2, "pressure");
  s.wind  = sensors.valueOf("wind");
  s.rain  = sensors.valueOf("rain");
  s.solar = sensors.valueOf("solar_wm2");

  hist_[histHead_] = s;
  histHead_ = (histHead_ + 1) % FC_HIST_MAX;
  if (histCount_ < FC_HIST_MAX) histCount_++;
}

const ForecastService::Sample* ForecastService::oldest() const {
  if (!histCount_) return nullptr;
  return &hist_[(histHead_ + FC_HIST_MAX - histCount_) % FC_HIST_MAX];
}

const ForecastService::Sample* ForecastService::sampleAt(size_t i) const {
  if (i >= histCount_) return nullptr;
  return &hist_[(histHead_ + FC_HIST_MAX - histCount_ + i) % FC_HIST_MAX];
}

time_t ForecastService::historyTime(size_t i) const {
  const Sample* s = sampleAt(i);
  return s ? s->t : 0;
}

float ForecastService::historyPressure(size_t i) const {
  const Sample* s = sampleAt(i);
  return s ? s->press : NAN;
}

float ForecastService::historyTemp(size_t i) const {
  const Sample* s = sampleAt(i);
  return s ? s->temp : NAN;
}

float ForecastService::historyHum(size_t i) const {
  const Sample* s = sampleAt(i);
  return s ? s->hum : NAN;
}

// Cisnienie sprzed "minut" (NAN, gdy historia jeszcze tak daleko nie siega).
float ForecastService::pressureAtAgeMinutes(int minutes) const {
  if (histCount_ < 2) return NAN;
  unsigned long now = millis();
  unsigned long want = (unsigned long)minutes * 60000UL;
  const Sample* best = nullptr;
  unsigned long bestDiff = 0xFFFFFFFFUL;
  for (size_t i = 0; i < histCount_; i++) {
    const Sample* s = sampleAt(i);
    if (!s || !ok(s->press)) continue;
    unsigned long age = now - s->ms;
    if (age < want) continue;
    unsigned long diff = age - want;
    if (diff < bestDiff) { bestDiff = diff; best = s; }
  }
  if (!best || bestDiff > 15UL * 60000UL) return NAN;
  return best->press;
}

void ForecastService::addLightning(float distanceKm, float energy) {
  strikes_[strikeHead_].ms = millis();
  strikes_[strikeHead_].dist = distanceKm;
  strikes_[strikeHead_].energy = energy;
  strikeHead_ = (strikeHead_ + 1) % 32;
  if (strikeCount_ < 32) strikeCount_++;
  LOG_W("Wyladowanie: %s km, energia %.0f",
        ok(distanceKm) ? String(distanceKm, 1).c_str() : "?", energy);
}

// ------------------------------------------------------------
//  Trend cisnienia
// ------------------------------------------------------------
void ForecastService::computeTrend() {
  info_.trendValid = false;
  info_.trend1h = NAN;
  info_.trend3h = NAN;
  info_.trendKey = "unknown";
  if (!ok(info_.pressHpa)) return;

  float p60 = pressureAtAgeMinutes(60);
  float p180 = pressureAtAgeMinutes(180);
  if (ok(p60))   info_.trend1h = info_.pressHpa - p60;      // hPa/h
  if (ok(p180))  info_.trend3h = info_.pressHpa - p180;     // hPa / 3 h
  if (!ok(info_.trend1h) && !ok(info_.trend3h)) return;

  info_.trendValid = true;
  float d = ok(info_.trend1h) ? info_.trend1h : info_.trend3h / 3.0f;
  if (d <= -1.0f)       info_.trendKey = "falling_fast";
  else if (d <= -0.3f)  info_.trendKey = "falling";
  else if (d < 0.3f)    info_.trendKey = "steady";
  else if (d < 1.0f)    info_.trendKey = "rising";
  else                  info_.trendKey = "rising_fast";
}

// ------------------------------------------------------------
//  Mgla i burza
// ------------------------------------------------------------
void ForecastService::computeFogAndStorm() {
  // ---------------- Mgla ----------------
  info_.fog = FogInfo();
  FogInfo& f = info_.fog;
  f.spreadC = (ok(info_.tempC) && ok(info_.dewPointC)) ? info_.tempC - info_.dewPointC : NAN;
  f.visibilityKm = sensors.valueOf("visibility");

  float risk = 0.0f;
  int parts = 0;
  if (ok(info_.humPct)) {
    parts++;
    if (info_.humPct >= 98.0f)      risk += 40.0f;
    else if (info_.humPct >= 95.0f) risk += 25.0f + (info_.humPct - 95.0f) * 3.0f;
    else if (info_.humPct >= 90.0f) risk += (info_.humPct - 90.0f) * 1.5f;
  }
  if (ok(f.spreadC)) {
    parts++;
    if (f.spreadC <= 0.0f)       risk += 28.0f;
    else if (f.spreadC <= 0.5f)  risk += 24.0f;
    else if (f.spreadC <= 1.0f)  risk += 16.0f;
    else if (f.spreadC <= 2.0f)  risk += 7.0f;
  }
  if (ok(f.visibilityKm)) {
    parts++;
    if (f.visibilityKm <= 0.5f)      risk += 28.0f;
    else if (f.visibilityKm <= 1.0f) risk += 22.0f;
    else if (f.visibilityKm <= 3.0f) risk += 14.0f;
    else if (f.visibilityKm <= 8.0f) risk += 6.0f;
    else                             risk += 1.0f;
  }
  if (ok(info_.windKmh)) {
    parts++;
    if (info_.windKmh <= 3.0f)       risk += 10.0f;
    else if (info_.windKmh <= 8.0f)  risk += 5.0f;
    else if (info_.windKmh >= 25.0f) risk *= 0.5f;   // wiatr rozprasza mgle
  }
  // Opad rozprasza mgle (i sam w sobie nie jest mgla)
  if (ok(info_.rainMm1h) && info_.rainMm1h > 0.2f) risk *= 0.3f;
  // W dzien, przy bezchmurnym niebie, mgla szybko sie wypala
  float elev = astro.valid() ? astro.sun().elevationDeg : NAN;
  if (ok(elev) && elev > 5.0f && ok(info_.cloudiness) && info_.cloudiness < 0.3f) risk *= 0.6f;

  f.risk = clampf(risk, 0.0f, 100.0f);
  f.active = parts >= 2 && f.risk >= 70.0f;

  // ---------------- Burza ----------------
  info_.storm = StormInfo();
  StormInfo& st = info_.storm;
  unsigned long now = millis();
  unsigned long minAge = 0xFFFFFFFFUL;
  st.strikesTotal = strikeCount_;

  for (size_t i = 0; i < strikeCount_; i++) {
    const Strike& k = strikes_[i];
    if (!k.ms) continue;
    unsigned long ageS = (now - k.ms) / 1000UL;
    if (ageS <= 900UL) {
      st.strikes15m++;
      if (ageS < minAge) {
        minAge = ageS;
        st.lastDistanceKm = k.dist;
        st.lastEnergy = k.energy;
      }
    }
    if (ageS <= 3600UL) st.strikes1h++;
  }
  st.lastStrikeAgeS = (minAge == 0xFFFFFFFFUL) ? 999 : minAge;

  float sRisk = 0.0f;
  if (st.strikes15m >= 3)      sRisk += 55.0f;
  else if (st.strikes15m >= 1) sRisk += 30.0f;
  if (ok(st.lastDistanceKm)) {
    if (st.lastDistanceKm <= 10.0f)      sRisk += 30.0f;
    else if (st.lastDistanceKm <= 20.0f) sRisk += 18.0f;
    else if (st.lastDistanceKm <= 40.0f) sRisk += 8.0f;
  } else if (st.strikes15m > 0) {
    sRisk += 10.0f;   // wyladowania sa, ale odleglosc poza zasiegiem detektora
  }
  if (ok(info_.trend1h)) {
    if (info_.trend1h <= -1.5f)      sRisk += 25.0f;
    else if (info_.trend1h <= -0.7f) sRisk += 12.0f;
  }
  if (ok(info_.windKmh)) {
    if (info_.windKmh >= 45.0f)      sRisk += 18.0f;
    else if (info_.windKmh >= 30.0f) sRisk += 10.0f;
  }
  if (ok(info_.rainMm1h)) {
    if (info_.rainMm1h >= 8.0f)      sRisk += 15.0f;
    else if (info_.rainMm1h >= 2.0f) sRisk += 8.0f;
  }
  st.risk = clampf(sRisk, 0.0f, 100.0f);
  st.nearby = st.strikes15m > 0 && ok(st.lastDistanceKm) && st.lastDistanceKm < 20.0f;
}

// ------------------------------------------------------------
//  Prognoza slowna (styl Zambrettiego: cisnienie + trend + wiatr)
// ------------------------------------------------------------
void ForecastService::computeOutlook() {
  int level = 0;                 // 1 = najlepiej, 6 = najgorzej, 0 = brak danych
  bool havePress = ok(info_.pressHpa);

  if (havePress) {
    if (info_.pressHpa >= 1025.0f)      level = 1;
    else if (info_.pressHpa >= 1018.0f) level = 2;
    else if (info_.pressHpa >= 1010.0f) level = 3;
    else if (info_.pressHpa >= 1003.0f) level = 4;
    else if (info_.pressHpa >= 995.0f)  level = 5;
    else                                level = 6;

    if (info_.trendValid) {
      if (!strcmp(info_.trendKey, "falling"))           level += 1;
      else if (!strcmp(info_.trendKey, "falling_fast")) level += 2;
      else if (!strcmp(info_.trendKey, "rising"))       level -= 1;
      else if (!strcmp(info_.trendKey, "rising_fast"))  level -= 1;
    }

    if (ok(info_.humPct) && info_.humPct >= 90.0f && level <= 4) level += 1;
    if (ok(info_.cloudiness) && info_.cloudiness >= 0.8f && level <= 4) level += 1;
    if (ok(info_.rainProbability) && info_.rainProbability >= 70.0f && level <= 4) level += 1;
    if (info_.storm.risk >= 60.0f) level = 6;
    level = (int)clampf((float)level, 1.0f, 6.0f);
  }

  info_.outlookLevel = level;
  switch (level) {
    case 1:  info_.outlookKey = "settled_fine";   break;
    case 2:  info_.outlookKey = "fair";           break;
    case 3:  info_.outlookKey = "changeable";     break;
    case 4:  info_.outlookKey = "unsettled";      break;
    case 5:  info_.outlookKey = "rain_soon";      break;
    case 6:  info_.outlookKey = "storm_warning";  break;
    default: info_.outlookKey = "unknown";        break;
  }

  // Zima, wysokie cisnienie i silny mroz - to osobna informacja
  if (ok(info_.tempC) && info_.tempC <= -10.0f && level >= 1 && level <= 2)
    info_.outlookKey = "hard_frost";
  if (info_.fog.active && level == 0) info_.outlookKey = "fog";

  float conf = 0.30f;
  if (havePress) conf += 0.15f;
  if (info_.trendValid) conf += 0.15f;
  if (ok(info_.humPct)) conf += 0.10f;
  if (ok(info_.tempC)) conf += 0.05f;
  if (ok(info_.windKmh)) conf += 0.05f;
  if (ok(info_.cloudiness)) conf += 0.10f;
  if (info_.storm.strikes1h > 0) conf += 0.10f;
  if (ok(info_.trend3h) && fabsf(info_.trend3h) >= 2.0f) conf += 0.05f;
  info_.confidence = clampf(conf, 0.0f, 0.95f);
}

// ------------------------------------------------------------
//  Wskazniki pochodne (punkt rosy, zachmurzenie, opad z 1 h)
// ------------------------------------------------------------
void ForecastService::computeDerived(time_t nowEpochUtc) {
  info_.clockValid = nowEpochUtc > 1600000000;
  info_.dewPointC = dewPoint(info_.tempC, info_.humPct);
  info_.absHumidity = absoluteHumidity(info_.tempC, info_.humPct);
  info_.heatIndexC = heatIndex(info_.tempC, info_.humPct);
  info_.windChillC = windChill(info_.tempC, info_.windKmh);
  info_.humidexC = humidex(info_.tempC, info_.humPct);

  // Opad z ostatniej godziny = roznica wskazan dobowego licznika deszczu
  float rainNow = sensors.valueOf("rain");
  info_.hasRain = ok(rainNow);
  info_.rainMm24h = ok(rainNow) ? rainNow : NAN;
  info_.rainMm1h = NAN;
  if (ok(rainNow)) {
    unsigned long now = millis();
    float ref = NAN;
    for (size_t i = 0; i < histCount_; i++) {
      const Sample* s = sampleAt(i);
      if (!s || !ok(s->rain)) continue;
      if ((now - s->ms) >= 3600000UL) { ref = s->rain; break; }
    }
    // Bez pelnej godziny historii zakladamy brak opadu (a nie 0 mm na dobe)
    info_.rainMm1h = ok(ref) ? fmaxf(0.0f, rainNow - ref) : 0.0f;
  }

  // Zachmurzenie z pyranometru albo luksomierza i wysokosci Slonca
  info_.cloudiness = NAN;
  info_.clearSkyWm2 = NAN;
  float elev = astro.valid() ? astro.sun().elevationDeg : NAN;
  if (ok(elev) && elev > 5.0f) {
    double sinAlt = sin(elev * PI / 180.0);
    info_.clearSkyWm2 = (float)(1050.0 * pow(sinAlt, 1.1));
    if (ok(info_.solarWm2) && info_.clearSkyWm2 > 50.0f) {
      info_.cloudiness = clampf(1.0f - info_.solarWm2 / info_.clearSkyWm2, 0.0f, 1.0f);
    } else {
      float lux = sensors.valueOf("light");
      float clearLux = (float)(120000.0 * sinAlt);
      if (ok(lux) && clearLux > 2000.0f)
        info_.cloudiness = clampf(1.0f - lux / clearLux, 0.0f, 1.0f);
    }
  }

  // Strona swiata wiatru
  static const char* const DIRS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  info_.windDirKey = "";
  if (ok(info_.windDirDeg)) {
    int idx = (int)((info_.windDirDeg + 22.5f) / 45.0f);
    if (idx < 0) idx += 8;
    info_.windDirKey = DIRS[idx % 8];
  }
}

// ------------------------------------------------------------
//  Glowne przeliczenie - wolane po kazdym odczycie czujnikow
// ------------------------------------------------------------
void ForecastService::update(time_t nowEpochUtc, int tzOffsetMin) {
  (void)tzOffsetMin;

  info_.tempC      = pickValue(T_IDS, 3, "temperature");
  info_.humPct     = pickValue(H_IDS, 2, "humidity");
  info_.pressHpa   = pickValue(P_IDS, 2, "pressure");
  info_.windKmh    = sensors.valueOf("wind");
  info_.windDirDeg = sensors.valueOf("vane");
  info_.uvIndex    = sensors.valueOf("uv");
  info_.solarWm2   = sensors.valueOf("solar_wm2");

  info_.hasTemp  = ok(info_.tempC);
  info_.hasHum   = ok(info_.humPct);
  info_.hasPress = ok(info_.pressHpa);
  info_.hasWind  = ok(info_.windKmh);
  info_.hasLight = ok(sensors.valueOf("light"));

  computeDerived(nowEpochUtc);
  computeTrend();
  computeFogAndStorm();

  // ---------------- Prawdopodobienstwo deszczu ----------------
  float p = 0.0f;
  int clues = 0;
  if (ok(info_.humPct)) {
    clues++;
    if (info_.humPct >= 92.0f)      p += 25.0f;
    else if (info_.humPct >= 85.0f) p += 18.0f;
    else if (info_.humPct >= 75.0f) p += 8.0f;
    else if (info_.humPct <= 50.0f) p -= 10.0f;
  }
  if (ok(info_.tempC) && ok(info_.dewPointC)) {
    clues++;
    float spread = info_.tempC - info_.dewPointC;
    if (spread <= 1.0f)       p += 25.0f;
    else if (spread <= 2.5f)  p += 15.0f;
    else if (spread <= 5.0f)  p += 6.0f;
    else                      p -= 5.0f;
  }
  if (info_.trendValid) {
    clues++;
    if (ok(info_.trend3h)) {
      if (info_.trend3h <= -2.5f)      p += 30.0f;
      else if (info_.trend3h <= -1.0f) p += 18.0f;
      else if (info_.trend3h >= 2.0f)  p -= 15.0f;
    } else if (ok(info_.trend1h) && info_.trend1h <= -0.7f) {
      p += 12.0f;
    }
  }
  if (ok(info_.pressHpa)) {
    clues++;
    if (info_.pressHpa < 1000.0f)       p += 12.0f;
    else if (info_.pressHpa >= 1025.0f) p -= 15.0f;
  }
  if (ok(info_.cloudiness)) {
    clues++;
    if (info_.cloudiness >= 0.85f)     p += 22.0f;
    else if (info_.cloudiness >= 0.6f) p += 12.0f;
    else if (info_.cloudiness <= 0.2f) p -= 12.0f;
  }
  if (ok(info_.rainMm1h) && info_.rainMm1h > 0.0f) { clues++; p += 15.0f; }
  if (info_.storm.strikes1h > 0) { clues++; p += 10.0f; }
  if (ok(info_.windKmh) && info_.windKmh >= 30.0f) p += 5.0f;

  info_.rainProbability = clues ? clampf(p, 0.0f, 95.0f) : NAN;
  if (!ok(info_.rainProbability))            info_.rainKey = "unknown";
  else if (info_.rainProbability < 10.0f)    info_.rainKey = "none";
  else if (info_.rainProbability < 30.0f)    info_.rainKey = "unlikely";
  else if (info_.rainProbability < 50.0f)    info_.rainKey = "possible";
  else if (info_.rainProbability < 70.0f)    info_.rainKey = "likely";
  else                                       info_.rainKey = "very_likely";
  // Deszcz juz pada - to nie prawdopodobienstwo, a fakt
  if (ok(info_.rainMm1h) && info_.rainMm1h > 0.0f) info_.rainKey = "raining";

  computeOutlook();

  info_.valid = clues > 0;
  info_.lastUpdateMs = millis();
}

// ------------------------------------------------------------
//  Lista czynnikow dla "szczegolowej analizy" na stronie www
// ------------------------------------------------------------
static void addFactor(ForecastFactor* arr, size_t& n, const char* key, float v, int weight) {
  if (n >= 24) return;
  arr[n].key = key;
  arr[n].value = v;
  arr[n].weight = weight;
  arr[n].available = ok(v);
  n++;
}

String ForecastService::analysisJson() const {
  ForecastFactor f[24];
  size_t n = 0;
  const ForecastInfo& i = info_;

  int wPress = 0;
  if (ok(i.pressHpa)) wPress = i.pressHpa >= 1015.0f ? 20 : (i.pressHpa < 1000.0f ? -25 : 0);
  int wTrend = 0;
  if (ok(i.trend3h)) wTrend = i.trend3h <= -2.0f ? -25 : (i.trend3h >= 2.0f ? 20 : 0);
  int wHum = 0;
  if (ok(i.humPct)) wHum = i.humPct >= 90.0f ? -20 : (i.humPct <= 45.0f ? 10 : 0);
  int wCloud = 0;
  if (ok(i.cloudiness)) wCloud = i.cloudiness >= 0.8f ? -20 : (i.cloudiness <= 0.2f ? 15 : 0);
  int wRain = ok(i.rainMm1h) && i.rainMm1h > 0.0f ? -20 : 0;
  float spread = (ok(i.tempC) && ok(i.dewPointC)) ? i.tempC - i.dewPointC : NAN;

  addFactor(f, n, "pressure", i.pressHpa, wPress);
  addFactor(f, n, "trend_3h", i.trend3h, wTrend);
  addFactor(f, n, "temperature", i.tempC, 0);
  addFactor(f, n, "humidity", i.humPct, wHum);
  addFactor(f, n, "dew_point", i.dewPointC, 0);
  addFactor(f, n, "dew_spread", spread, 0);
  addFactor(f, n, "wind", i.windKmh, 0);
  addFactor(f, n, "rain_1h", i.rainMm1h, wRain);
  addFactor(f, n, "cloudiness", ok(i.cloudiness) ? i.cloudiness * 100.0f : NAN, wCloud);
  addFactor(f, n, "solar", i.solarWm2, 0);
  addFactor(f, n, "uv", i.uvIndex, 0);
  addFactor(f, n, "visibility", i.fog.visibilityKm, 0);
  addFactor(f, n, "fog_risk", i.fog.risk, i.fog.risk >= 70.0f ? -30 : 0);
  addFactor(f, n, "lightning", (float)i.storm.strikes15m, i.storm.strikes15m ? -35 : 0);

  String j = "{\"factors\":[";
  for (size_t k = 0; k < n; k++) {
    if (k) j += ",";
    j += "{\"key\":\"";
    j += f[k].key;
    j += "\",\"available\":";
    j += f[k].available ? "true" : "false";
    j += ",\"value\":";
    j += f[k].available ? String(f[k].value, 2) : String("null");
    j += ",\"weight\":";
    j += String(f[k].weight);
    j += "}";
  }
  j += "],\"trend_key\":\"";
  j += info_.trendKey;
  j += "\",\"rain_key\":\"";
  j += info_.rainKey;
  j += "\",\"outlook_key\":\"";
  j += info_.outlookKey;
  j += "\"}";
  return j;
}

String ForecastService::json() const {
  const ForecastInfo& i = info_;
  String j = "{\"valid\":";
  j += i.valid ? "true" : "false";
  j += ",\"clock_valid\":";
  j += i.clockValid ? "true" : "false";
  j += ",\"confidence\":";
  j += String(i.confidence * 100.0f, 0);
  j += ",\"outlook_key\":\"";
  j += i.outlookKey;
  j += "\",\"outlook_level\":";
  j += String(i.outlookLevel);

  j += ",\"trend\":{\"valid\":";
  j += i.trendValid ? "true" : "false";
  j += ",\"key\":\"";
  j += i.trendKey;
  j += "\",\"h1\":";
  j += ok(i.trend1h) ? String(i.trend1h, 2) : String("null");
  j += ",\"h3\":";
  j += ok(i.trend3h) ? String(i.trend3h, 2) : String("null");
  j += "}";

  j += ",\"derived\":{\"dew_point\":";
  j += ok(i.dewPointC) ? String(i.dewPointC, 1) : String("null");
  j += ",\"abs_humidity\":";
  j += ok(i.absHumidity) ? String(i.absHumidity, 1) : String("null");
  j += ",\"heat_index\":";
  j += ok(i.heatIndexC) ? String(i.heatIndexC, 1) : String("null");
  j += ",\"wind_chill\":";
  j += ok(i.windChillC) ? String(i.windChillC, 1) : String("null");
  j += ",\"humidex\":";
  j += ok(i.humidexC) ? String(i.humidexC, 1) : String("null");
  j += ",\"cloudiness\":";
  j += ok(i.cloudiness) ? String(i.cloudiness * 100.0f, 0) : String("null");
  j += ",\"clear_sky_wm2\":";
  j += ok(i.clearSkyWm2) ? String(i.clearSkyWm2, 0) : String("null");
  j += "}";

  j += ",\"rain\":{\"probability\":";
  j += ok(i.rainProbability) ? String(i.rainProbability, 0) : String("null");
  j += ",\"key\":\"";
  j += i.rainKey;
  j += "\",\"mm_1h\":";
  j += ok(i.rainMm1h) ? String(i.rainMm1h, 1) : String("null");
  j += ",\"mm_today\":";
  j += ok(i.rainMm24h) ? String(i.rainMm24h, 1) : String("null");
  j += "}";

  j += ",\"fog\":{\"risk\":";
  j += String(i.fog.risk, 0);
  j += ",\"active\":";
  j += i.fog.active ? "true" : "false";
  j += ",\"spread\":";
  j += ok(i.fog.spreadC) ? String(i.fog.spreadC, 1) : String("null");
  j += ",\"visibility\":";
  j += ok(i.fog.visibilityKm) ? String(i.fog.visibilityKm, 1) : String("null");
  j += "}";

  j += ",\"storm\":{\"risk\":";
  j += String(i.storm.risk, 0);
  j += ",\"nearby\":";
  j += i.storm.nearby ? "true" : "false";
  j += ",\"strikes_15m\":";
  j += String(i.storm.strikes15m);
  j += ",\"strikes_1h\":";
  j += String(i.storm.strikes1h);
  j += ",\"strikes_total\":";
  j += String(i.storm.strikesTotal);
  j += ",\"last_distance\":";
  j += ok(i.storm.lastDistanceKm) ? String(i.storm.lastDistanceKm, 1) : String("null");
  j += ",\"last_energy\":";
  j += ok(i.storm.lastEnergy) ? String(i.storm.lastEnergy, 0) : String("null");
  j += ",\"last_age_s\":";
  j += String(i.storm.lastStrikeAgeS);
  j += "}";

  j += ",\"sources\":{\"temp\":";
  j += i.hasTemp ? "true" : "false";
  j += ",\"hum\":";
  j += i.hasHum ? "true" : "false";
  j += ",\"press\":";
  j += i.hasPress ? "true" : "false";
  j += ",\"wind\":";
  j += i.hasWind ? "true" : "false";
  j += ",\"rain\":";
  j += i.hasRain ? "true" : "false";
  j += ",\"light\":";
  j += i.hasLight ? "true" : "false";
  j += "}";

  j += ",\"values\":{\"temp\":";
  j += ok(i.tempC) ? String(i.tempC, 1) : String("null");
  j += ",\"hum\":";
  j += ok(i.humPct) ? String(i.humPct, 0) : String("null");
  j += ",\"press\":";
  j += ok(i.pressHpa) ? String(i.pressHpa, 1) : String("null");
  j += ",\"wind\":";
  j += ok(i.windKmh) ? String(i.windKmh, 1) : String("null");
  j += ",\"solar\":";
  j += ok(i.solarWm2) ? String(i.solarWm2, 0) : String("null");
  j += ",\"uv\":";
  j += ok(i.uvIndex) ? String(i.uvIndex, 1) : String("null");
  j += ",\"wind_dir_deg\":";
  j += ok(i.windDirDeg) ? String(i.windDirDeg, 0) : String("null");
  j += "}";

  j += ",\"wind_dir\":\"";
  j += i.windDirKey;
  j += "\"}";
  return j;
}
