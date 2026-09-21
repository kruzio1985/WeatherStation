/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "analysis.h"
#include "aqi.h"
#include "logger.h"
#include "sd_card.h"
#include "syslog.h"
#include <time.h>
#include <math.h>
#include <esp_task_wdt.h>

Analysis analysis;

// ------------------------------------------------------------
//  Pomocnicze
// ------------------------------------------------------------
static long dayStartTs(time_t ts) {
  struct tm tmv;
  localtime_r(&ts, &tmv);
  struct tm b = tmv;
  b.tm_hour = 0; b.tm_min = 0; b.tm_sec = 0;
  return (long)mktime(&b);
}

static String dtString(time_t ts) {
  if (ts < 1000000000) return "-";
  struct tm tmv;
  localtime_r(&ts, &tmv);
  char b[20];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min);
  return String(b);
}

static String fmt(float v) {
  char b[16];
  dtostrf(v, 0, 2, b);
  return String(b);
}

struct MetricLabel { const char* id; const char* name; const char* unit; };
static const MetricLabel LABELS[] = {
  { "temp", "Temperatura", "°C" },
  { "hum",  "Wilgotność", "%" },
  { "press","Ciśnienie", "hPa" },
  { "light","Natężenie światła", "lx" },
  { "wind", "Wiatr", "km/h" },
  { "vane", "Kierunek wiatru", "°" },
  { "rain", "Opad (dziś)", "mm" },
  { "pm1",  "PM1.0", "µg/m³" },
  { "pm25", "PM2.5", "µg/m³" },
  { "pm10", "PM10", "µg/m³" },
  { "co2",  "CO₂", "ppm" },
  { "eco2", "eCO₂", "ppm" },
  { "tvoc", "TVOC", "ppb" },
  { "uv",   "Indeks UV", "UVI" },
  { "solar_wm2", "Promieniowanie", "W/m²" },
  { "soil", "Wilgotność gleby", "%" },
  { "mmc_hdg", "Kurs (kompas)", "°" },
  { "sht_t", "Temperatura SHT", "°C" },
  { "sht_h", "Wilgotność SHT", "%" },
  { "bmp_t", "Temperatura BMP", "°C" },
  { "bmp_p", "Ciśnienie BMP", "hPa" },
  { "sen_t", "Temperatura SEN", "°C" },
  { "sen_h", "Wilgotność SEN", "%" },
  { "sen_voc", "VOC (SEN)", "" },
  { "sen_nox", "NOx (SEN)", "" },
  { "in_pm1",  "PM1.0 (wewn.)", "µg/m³" },
  { "in_pm25", "PM2.5 (wewn.)", "µg/m³" },
  { "in_pm4",  "PM4.0 (wewn.)", "µg/m³" },
  { "in_pm10", "PM10 (wewn.)", "µg/m³" },
};

static void metricLabel(const String& id, String& name, String& unit) {
  String base = id;
  String prefix;
  if (SensorManager::isRemoteId(id)) {
    int us = id.indexOf('_');
    if (us > 1) {
      prefix = "[" + id.substring(1, us) + "] ";
      base = id.substring(us + 1);
    }
  }
  for (auto& L : LABELS) {
    if (base == L.id) { name = prefix + L.name; unit = L.unit; return; }
  }
  if (base.startsWith("ds_"))  { name = prefix + "Czujnik DS18B20 #" + base.substring(3); unit = "°C"; return; }
  if (base.startsWith("ads_")) { name = prefix + "ADC #" + base.substring(4); unit = ""; return; }
  name = id;
  unit = "";
}

// ------------------------------------------------------------
//  Skanowanie szerokiego CSV (timestamp,datetime,kolumna...)
// ------------------------------------------------------------
typedef void (*WideCb)(long ts, int colIdx, const String& col, float v, void* ctx);

static void parseWideLine(const String& line, bool header, WideCb cb, void* ctx) {
  if (header) return;
  int c1 = line.indexOf(',');
  if (c1 < 0) return;
  long ts = atol(line.substring(0, c1).c_str());

  int f = 0, start = 0, idx = c1 + 1;
  while (idx <= (int)line.length()) {
    if (idx == (int)line.length() || line[idx] == ',') {
      if (f >= 2) {
        String v = line.substring(start, idx);
        if (v.length() > 0) {
          float fv = atof(v.c_str());
          if (!isnan(fv)) cb(ts, f - 2, DataLogger::columnName(f - 2), fv, ctx);
        }
      }
      f++;
      start = idx + 1;
    }
    idx++;
  }
}

// Skanuje szeroki CSV strumieniowo (timestamp,datetime,kolumna...). Nie
// wczytuje pliku do RAM-u - karta działa na 4 MHz, a plik miesięczny bywa
// duży. Co kilka wierszy oddaje sterowanie, żeby nie wyzwolić watchdoga.
static void scanWideFile(const String& path, WideCb cb, void* ctx, size_t maxBytes) {
  File f = sdCard.openRead(path);
  if (!f) return;

  char buf[512];
  String line;
  line.reserve(256);
  bool header = true;
  size_t total = 0;
  int lines = 0;
  while (f.available() && total < maxBytes) {
    int n = f.read((uint8_t*)buf, sizeof(buf));
    if (n <= 0) break;
    total += (size_t)n;
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n') {
        if (line.length() && line[line.length() - 1] == '\r') line.remove(line.length() - 1);
        line.trim();
        if (line.length()) parseWideLine(line, header, cb, ctx);
        line = "";
        header = false;
        if ((++lines & 0x3F) == 0) { yield(); esp_task_wdt_reset(); }
      } else {
        line += c;
      }
    }
  }
  if (line.length()) {
    if (line[line.length() - 1] == '\r') line.remove(line.length() - 1);
    line.trim();
    if (line.length()) parseWideLine(line, header, cb, ctx);
  }
  f.close();
}

// ------------------------------------------------------------
//  Rekordy (ekstrema)
// ------------------------------------------------------------
void Analysis::noteValue(time_t ts, const String& metric, float v) {
  if (isnan(v) || metric.length() == 0) return;

  int idx = -1;
  for (size_t i = 0; i < metricOrder_.size(); i++) {
    if (metricOrder_[i] == metric) { idx = (int)i; break; }
  }
  if (idx < 0) {
    metricOrder_.push_back(metric);
    metricRec_.push_back(MetricRec());
    idx = (int)metricOrder_.size() - 1;
  }

  MetricRec& r = metricRec_[idx];
  if (isnan(r.mn) || v < r.mn) { r.mn = v; r.mnTs = ts; }
  if (isnan(r.mx) || v > r.mx) { r.mx = v; r.mxTs = ts; }

  // Opad: wartość to licznik dobowy (mm "od zera"), więc maksimum z dnia
  // to suma dobowa - z tego liczymy rekord "maksymalny opad dobowy".
  if (metric == "rain" || metric.endsWith("_rain")) {
    long ds = dayStartTs(ts);
    int di = -1;
    for (size_t i = 0; i < rainDayTs_.size(); i++) {
      if (rainDayTs_[i] == ds) { di = (int)i; break; }
    }
    if (di < 0) { rainDayTs_.push_back(ds); rainDayMm_.push_back(v); }
    else if (v > rainDayMm_[di]) rainDayMm_[di] = v;
  }
}

void Analysis::noteSamples(const std::vector<Channel>& channels) {
  time_t now = time(nullptr);
  if (now < 1000000000) return;
  if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
  for (const auto& c : channels) {
    if (!c.enabled || isnan(c.value)) continue;
    noteValue(now, c.id, c.value);
  }
  xSemaphoreGive(mutex_);
}

static void seedCb(long ts, int, const String& col, float v, void* ctx) {
  ((Analysis*)ctx)->noteValue((time_t)ts, col, v);
}

bool Analysis::seedNext() {
  if (seedFiles_.empty()) {
    std::vector<SdFileInfo> files;
    sdCard.listDir("/logs", files);
    for (auto& f : files) {
      String name = f.path;
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.endsWith(".csv")) continue;
      if (name.startsWith("extra-") || name.startsWith(".")) continue;
      seedFiles_.push_back(f.path);
    }
    seedIdx_ = 0;
  }
  if (seedIdx_ >= (int)seedFiles_.size()) return true;

  scanWideFile(seedFiles_[seedIdx_], seedCb, this, 8 * 1024 * 1024);
  seedIdx_++;
  return seedIdx_ >= (int)seedFiles_.size();
}

void Analysis::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
}

void Analysis::loop() {
  // Faza 1: dosiewanie rekordów z archiwum CSV (jeden plik na obieg pętli).
  if (!seeded_) {
    if (!sdCard.mounted()) return;
    if (time(nullptr) < 1000000000) return;
    if (millis() - lastSeedMs_ < 250) return;
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
    bool done = seedNext();
    xSemaphoreGive(mutex_);
    lastSeedMs_ = millis();
    if (done) {
      seeded_ = true;
      LOG_I("Analiza: rekordy dosiane z %d plików CSV", seedIdx_);
      cacheValid_ = false;  // przebuduj pamięć podręczną po dosianiu
    }
    return;
  }

  // Faza 2: okresowa przebudowa podsumowania (rekordy + dziś + ciśnienie).
  if (cacheValid_ && millis() - lastBuildMs_ < 60000UL) return;
  buildCache();
  lastBuildMs_ = millis();
  cacheValid_ = true;
}

// ------------------------------------------------------------
//  JSON
// ------------------------------------------------------------
void Analysis::recordsJson(String& out) {
  out += "\"records\":[";
  bool first = true;
  for (size_t i = 0; i < metricOrder_.size(); i++) {
    const String& m = metricOrder_[i];
    const MetricRec& r = metricRec_[i];
    // Dla deszczu min/max licznika dobowego nie ma sensu - osobne pole niżej.
    if (m == "rain" || m.endsWith("_rain")) continue;
    if (isnan(r.mn) && isnan(r.mx)) continue;

    if (!first) out += ",";
    first = false;
    String name, unit;
    metricLabel(m, name, unit);
    out += "{\"id\":\""; out += m; out += "\"";
    out += ",\"name\":\""; out += name; out += "\"";
    out += ",\"unit\":\""; out += unit; out += "\"";
    if (!isnan(r.mn)) { out += ",\"min\":"; out += fmt(r.mn); out += ",\"min_dt\":\""; out += dtString(r.mnTs); out += "\""; }
    if (!isnan(r.mx)) { out += ",\"max\":"; out += fmt(r.mx); out += ",\"max_dt\":\""; out += dtString(r.mxTs); out += "\""; }
    out += "}";
  }
  out += "]";

  // Maksymalny opad dobowy
  float rmax = NAN;
  long rmaxTs = 0;
  for (size_t i = 0; i < rainDayMm_.size(); i++) {
    if (isnan(rmax) || rainDayMm_[i] > rmax) { rmax = rainDayMm_[i]; rmaxTs = rainDayTs_[i]; }
  }
  out += ",\"rain_max_day\":";
  if (isnan(rmax)) out += "null";
  else {
    out += fmt(rmax);
    out += ",\"rain_max_day_dt\":\""; out += dtString((time_t)rmaxTs); out += "\"";
  }
}

struct TodayAgg { float sum = 0, mn = 1e30f, mx = -1e30f; int n = 0; };

static void todayCb(long, int colIdx, const String&, float v, void* ctx) {
  auto* aggs = (std::vector<TodayAgg>*)ctx;
  if (colIdx < 0 || colIdx >= (int)aggs->size()) return;
  TodayAgg& a = (*aggs)[colIdx];
  a.sum += v; a.n++;
  if (v < a.mn) a.mn = v;
  if (v > a.mx) a.mx = v;
}

void Analysis::todayJson(String& out) {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char pathBuf[24];
  snprintf(pathBuf, sizeof(pathBuf), "/logs/%04d-%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1);

  out += "\"today\":";
  std::vector<TodayAgg> aggs(DataLogger::columnCount());
  scanWideFile(String(pathBuf), todayCb, &aggs, 8 * 1024 * 1024);

  static const char* TODAY_METRICS[] = {
    "temp", "hum", "press", "wind", "pm25", "co2", "uv", "light", "rain"
  };

  out += "{\"date\":\"";
  out += String(pathBuf).substring(6); // "YYYY-MM.csv"
  out += "\",\"metrics\":[";
  bool first = true;
  for (const char* mname : TODAY_METRICS) {
    // znajdź kolumnę
    int ci = -1;
    for (int i = 0; i < DataLogger::columnCount(); i++) {
      if (DataLogger::columnName(i) == mname) { ci = i; break; }
    }
    if (ci < 0) continue;
    const TodayAgg& a = aggs[ci];
    if (a.n == 0) continue;

    if (!first) out += ",";
    first = false;
    String name, unit;
    metricLabel(String(mname), name, unit);
    out += "{\"id\":\""; out += mname; out += "\"";
    out += ",\"name\":\""; out += name; out += "\"";
    out += ",\"unit\":\""; out += unit; out += "\"";
    out += ",\"avg\":"; out += fmt(a.sum / a.n);
    out += ",\"min\":"; out += fmt(a.mn);
    out += ",\"max\":"; out += fmt(a.mx);
    out += ",\"n\":"; out += String(a.n);
    out += "}";
  }
  out += "]}";
}

void Analysis::pressureTrendJson(String& out) {
  std::vector<SeriesPoint> day, week;
  String e1, e2;
  bool okDay = logger.getSeries("day", "press", day, e1);
  bool okWeek = logger.getSeries("week", "press", week, e2);

  float nowP = NAN, p3h = NAN, p24h = NAN;

  if (okDay && !day.empty()) {
    nowP = day.back().avg;
    long target = (long)time(nullptr) - 3 * 3600;
    long bestDiff = 0x7fffffffL;
    for (auto& p : day) {
      long d = p.ts > target ? p.ts - target : target - p.ts;
      if (d < bestDiff) { bestDiff = d; p3h = p.avg; }
    }
  }
  if (okWeek && !week.empty()) {
    // przedostatni punkt = wczoraj (ostatni to dzisiaj, jeszcze niepełny)
    p24h = week.size() >= 2 ? week[week.size() - 2].avg : NAN;
  }

  out += "\"pressure_trend\":{";
  out += "\"now\":";   out += isnan(nowP) ? "null" : fmt(nowP);
  out += ",\"h3\":";   out += isnan(p3h)  ? "null" : fmt(p3h);
  out += ",\"h24\":";  out += isnan(p24h) ? "null" : fmt(p24h);
  if (!isnan(nowP) && !isnan(p3h))  { out += ",\"delta3h\":";  out += fmt(nowP - p3h); }
  if (!isnan(nowP) && !isnan(p24h)) { out += ",\"delta24h\":"; out += fmt(nowP - p24h); }
  out += "}";
}

// ------------------------------------------------------------
//  Róża wiatrów (kierunek "vane" + prędkość "wind" z szerokiego CSV)
// ------------------------------------------------------------
static const char* WIND_DIRS[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
static const char* WIND_DIRS_PL[8] = {
  "Północny", "Północno-wschodni", "Wschodni", "Południowo-wschodni",
  "Południowy", "Południowo-zachodni", "Zachodni", "Północno-zachodni"
};
// Wiatr poniżej progu traktujemy jako ciszę (kierunek nie ma znaczenia).
static const float WIND_CALM_KMH = 1.0f;

static long periodFrom(const String& period) {
  time_t now = time(nullptr);
  if (now < 1000000000) now = 0;
  if (period == "day")   return (long)now - 86400;
  if (period == "week")  return (long)now - 7 * 86400;
  if (period == "month") return (long)now - 30 * 86400;
  if (period == "year")  return (long)now - 365 * 86400;
  return (long)now - 86400;
}

struct WindRoseCtx {
  long from = 0;
  long lastTs = -1;
  bool haveVane = false; float vane = 0;
  bool haveWind = false; float wind = 0;

  int    secCount[8] = {0};
  double secSpeedSum[8] = {0};
  float  secMax[8] = {0};
  int    calm = 0;
  int    total = 0;      // próbki z kierunkiem (suma secCount)
  int    samples = 0;    // wszystkie wiersze z danymi wiatru/kierunku

  void finalize() {
    if (!haveVane && !haveWind) return;
    samples++;
    bool isCalm = haveWind && wind < WIND_CALM_KMH;
    if (isCalm) { calm++; return; }
    if (!haveVane) return; // jest prędkość, brak kierunku - nie da się przyporządkować
    int s = ((int)floorf((vane + 22.5f) / 45.0f)) % 8;
    if (s < 0) s += 8;
    secCount[s]++;
    total++;
    if (haveWind) {
      secSpeedSum[s] += wind;
      if (wind > secMax[s]) secMax[s] = wind;
    }
  }
};

static void windRoseCb(long ts, int, const String& col, float v, void* ctx) {
  auto* c = (WindRoseCtx*)ctx;
  if (ts < c->from) return;
  if (ts != c->lastTs) {
    c->finalize();
    c->lastTs = ts;
    c->haveVane = c->haveWind = false;
  }
  if (col == "vane") {
    if (v < 0 || v > 360) return;
    c->vane = v; c->haveVane = true;
  } else if (col == "wind") {
    if (v < 0) return;
    c->wind = v; c->haveWind = true;
  }
}

String Analysis::windRoseJson(const String& period) {
  if (!sdCard.mounted()) return "{\"error\":\"Brak karty SD\"}";

  WindRoseCtx ctx;
  ctx.from = periodFrom(period);

  std::vector<SdFileInfo> files;
  sdCard.listDir("/logs", files);

  time_t now = time(nullptr);
  for (auto& fi : files) {
    String name = fi.path;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".csv") || name.startsWith(".") || name.startsWith("extra-")) continue;

    // Wstępny filtr po nazwie miesiąca (YYYY-MM.csv).
    int y, m;
    if (sscanf(name.c_str(), "%d-%d", &y, &m) == 2) {
      struct tm tmv = {};
      tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = 1;
      time_t fileTs = mktime(&tmv);
      if (fileTs < (time_t)(ctx.from - 32 * 86400) || fileTs > now) continue;
    }
    scanWideFile(fi.path, windRoseCb, &ctx, 8 * 1024 * 1024);
    esp_task_wdt_reset();
  }
  ctx.finalize();

  String out;
  out.reserve(1024);
  out += "{\"period\":\""; out += period; out += "\"";
  out += ",\"calm\":"; out += String(ctx.calm);
  out += ",\"total\":"; out += String(ctx.total);
  out += ",\"samples\":"; out += String(ctx.samples);
  out += ",\"sectors\":[";
  for (int i = 0; i < 8; i++) {
    if (i) out += ",";
    float pct = ctx.total ? 100.0f * (float)ctx.secCount[i] / (float)ctx.total : 0.0f;
    float spd = ctx.secCount[i] ? (float)(ctx.secSpeedSum[i] / ctx.secCount[i]) : 0.0f;
    out += "{\"dir\":\""; out += WIND_DIRS[i]; out += "\"";
    out += ",\"label\":\""; out += WIND_DIRS_PL[i]; out += "\"";
    out += ",\"count\":"; out += String(ctx.secCount[i]);
    out += ",\"pct\":"; out += fmt(pct);
    out += ",\"speed\":"; out += fmt(spd);
    out += ",\"max\":"; out += (ctx.secMax[i] > 0 ? fmt(ctx.secMax[i]) : "0.00");
    out += "}";
  }
  out += "]}";
  return out;
}

// ------------------------------------------------------------
//  Indeks jakości powietrza (CAQI) z PM2.5 / PM10
// ------------------------------------------------------------
struct AqiDayCtx {
  float sum25 = 0, sum10 = 0;
  float mx25 = NAN, mx10 = NAN;
  int n25 = 0, n10 = 0;
};

static void aqiDayCb(long, int, const String& col, float v, void* ctx) {
  auto* c = (AqiDayCtx*)ctx;
  if (col == "pm25") {
    c->sum25 += v; c->n25++;
    if (isnan(c->mx25) || v > c->mx25) c->mx25 = v;
  } else if (col == "pm10") {
    c->sum10 += v; c->n10++;
    if (isnan(c->mx10) || v > c->mx10) c->mx10 = v;
  }
}

static void aqiNum(String& out, float v) {
  if (isnan(v)) out += "null";
  else out += fmt(v);
}

String Analysis::aqiJson() {
  // Bieżące wartości z kanałów pomiarowych (NAN = brak).
  float pm25 = sensors.valueOf("pm25");
  float pm10 = sensors.valueOf("pm10");
  float co2 = sensors.valueOf("co2");
  if (isnan(co2)) co2 = sensors.valueOf("eco2");

  AqiResult live = aqiFromPm(pm25, pm10);

  // Statystyki dnia z dzisiejszego pliku CSV.
  AqiDayCtx day;
  if (sdCard.mounted()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char pathBuf[24];
    snprintf(pathBuf, sizeof(pathBuf), "/logs/%04d-%02d.csv",
             tmv.tm_year + 1900, tmv.tm_mon + 1);
    scanWideFile(String(pathBuf), aqiDayCb, &day, 8 * 1024 * 1024);
  }

  float avg25 = day.n25 ? day.sum25 / day.n25 : NAN;
  float avg10 = day.n10 ? day.sum10 / day.n10 : NAN;
  AqiResult dayIdx = aqiFromPm(avg25, avg10);

  String out;
  out.reserve(1024);
  out += "{\"available\":";
  out += live.available ? "true" : "false";
  out += ",\"source\":\"PM2.5 / PM10 (pyłomierz)\"";

  out += ",\"pm25\":";
  aqiNum(out, pm25);
  out += ",\"pm10\":";
  aqiNum(out, pm10);
  out += ",\"index\":";
  out += String(live.index);
  out += ",\"category\":\"";
  out += live.category;
  out += "\",\"color\":\"";
  out += live.color;
  out += "\",\"advice\":\"";
  out += live.advice;
  out += "\"";

  // CO2 wewnątrz (SCD4x / SGP30) - klasyfikacja jakości powietrza w pomieszczeniu.
  out += ",\"co2\":";
  aqiNum(out, co2);
  out += ",\"co2_quality\":\"";
  out += co2Quality(co2);
  out += "\",\"co2_color\":\"";
  out += co2QualityColor(co2);
  out += "\"";

  out += ",\"day\":{";
  out += "\"pm25_avg\":";
  aqiNum(out, avg25);
  out += ",\"pm25_max\":";
  aqiNum(out, day.mx25);
  out += ",\"pm10_avg\":";
  aqiNum(out, avg10);
  out += ",\"pm10_max\":";
  aqiNum(out, day.mx10);
  out += ",\"index\":";
  out += String(dayIdx.index);
  out += ",\"category\":\"";
  out += dayIdx.category;
  out += "\",\"color\":\"";
  out += dayIdx.color;
  out += "\"}";
  out += "}";
  return out;
}

// Buduje cały JSON w pętli głównej (nie w kontekście żądania HTTP) i podmienia
// pamięć podręczną pod mutexem. Żądanie /api/analysis czyta wyłącznie cache.
void Analysis::buildCache() {
  String out;
  out.reserve(8192);
  out += "{\"seeded\":";
  out += seeded_ ? "true" : "false";
  out += ",";
  recordsJson(out);
  out += ",";
  esp_task_wdt_reset();
  todayJson(out);
  out += ",";
  esp_task_wdt_reset();
  pressureTrendJson(out);
  out += "}";

  if (!mutex_) return;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
  cache_ = out;
  xSemaphoreGive(mutex_);
}

String Analysis::json() {
  String out;
  out += "{\"seeded\":";
  out += seeded_ ? "true" : "false";
  if (!cacheValid_) {
    out += ",\"busy\":true}";
    return out;
  }
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) {
    out += ",\"busy\":true}";
    return out;
  }
  out = cache_;
  xSemaphoreGive(mutex_);
  return out;
}
