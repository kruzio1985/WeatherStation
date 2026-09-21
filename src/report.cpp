/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "report.h"
#include "config.h"
#include "logger.h"
#include "sd_card.h"
#include "syslog.h"
#include <time.h>
#include <math.h>
#include <map>
#include <vector>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>

ReportService reports;

// Wskaźniki pokazywane w raporcie (kolejność kolumn na stronie i w CSV).
static const char* METRICS[] = {
  "temp", "hum", "press", "wind", "rain", "light", "uv", "pm25", "pm10", "co2"
};
static const int NMETRICS = sizeof(METRICS) / sizeof(METRICS[0]);

struct MetricLabel { const char* id; const char* name; const char* unit; };
static const MetricLabel LABELS[] = {
  { "temp", "Temperatura", "°C" },
  { "hum",  "Wilgotność", "%" },
  { "press","Ciśnienie", "hPa" },
  { "wind", "Wiatr", "km/h" },
  { "rain", "Opad (dziś)", "mm" },
  { "light","Światło", "lx" },
  { "uv",   "Indeks UV", "UVI" },
  { "pm25", "PM2.5", "µg/m³" },
  { "pm10", "PM10", "µg/m³" },
  { "co2",  "CO₂", "ppm" },
};

static String metricName(const char* id) {
  for (auto& L : LABELS) if (strcmp(L.id, id) == 0) return String(L.name);
  return String(id);
}
static String metricUnit(const char* id) {
  for (auto& L : LABELS) if (strcmp(L.id, id) == 0) return String(L.unit);
  return String("");
}

static long dayStartTs(time_t ts) {
  struct tm tmv;
  localtime_r(&ts, &tmv);
  struct tm b = tmv;
  b.tm_hour = 0; b.tm_min = 0; b.tm_sec = 0;
  return (long)mktime(&b);
}
static long weekStartTs(time_t ts) {
  struct tm tmv;
  localtime_r(&ts, &tmv);
  struct tm b = tmv;
  b.tm_hour = 0; b.tm_min = 0; b.tm_sec = 0;
  int wday = b.tm_wday == 0 ? 7 : b.tm_wday;   // poniedziałek = 1
  b.tm_mday -= (wday - 1);
  return (long)mktime(&b);
}
static long monthStartTs(time_t ts) {
  struct tm tmv;
  localtime_r(&ts, &tmv);
  struct tm b = tmv;
  b.tm_hour = 0; b.tm_min = 0; b.tm_sec = 0;
  b.tm_mday = 1;
  return (long)mktime(&b);
}
static long bucketStart(time_t ts, const String& period) {
  if (period == "month") return monthStartTs(ts);
  if (period == "week")  return weekStartTs(ts);
  return dayStartTs(ts);
}
static String bucketLabel(long ts, const String& period) {
  struct tm tmv;
  localtime_r((const time_t*)&ts, &tmv);
  char b[16];
  if (period == "month") snprintf(b, sizeof(b), "%04d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
  else if (period == "week") snprintf(b, sizeof(b), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  else snprintf(b, sizeof(b), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return String(b);
}

struct Agg {
  float sum = 0, mn = NAN, mx = NAN;
  int n = 0;
  void add(float v) {
    sum += v; n++;
    if (isnan(mn) || v < mn) mn = v;
    if (isnan(mx) || v > mx) mx = v;
  }
  float avg() const { return n ? sum / n : NAN; }
};

struct Bucket {
  long ts = 0;
  Agg m[NMETRICS];
};

// Mapowanie indeksu kolumny szerokiego CSV -> indeks wskaźnika raportu
static void buildColMap(std::map<int, int>& colMap) {
  for (int i = 0; i < NMETRICS; i++) {
    String col = logger.columnForMetric(METRICS[i]);
    for (int c = 0; c < DataLogger::columnCount(); c++) {
      if (DataLogger::columnName(c) == col) { colMap[c] = i; break; }
    }
  }
}

static long fileMonthTs(const String& name) {
  // name = "YYYY-MM.csv"
  int y = 0, m = 0;
  if (sscanf(name.c_str(), "%d-%d", &y, &m) != 2) return 0;
  struct tm tmv = {};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = m - 1;
  tmv.tm_mday = 1;
  return (long)mktime(&tmv);
}

static bool compute(const String& period, std::vector<Bucket>& out, long& from, long& to) {
  time_t now = time(nullptr);
  if (now < 1000000000) return false;   // brak synchronizacji czasu
  to = (long)now;

  if (period == "month") from = now - 366L * 86400;
  else if (period == "week") from = now - 84L * 86400;
  else from = now - 31L * 86400;

  std::map<int, int> colMap;
  buildColMap(colMap);

  std::map<long, int> bucketIdx;
  std::vector<SdFileInfo> files;
  sdCard.listDir("/logs", files);

  for (auto& fi : files) {
    String name = fi.path;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".csv") || name.startsWith("extra-") || name.startsWith(".")) continue;
    // Pomijamy pliki spoza zakresu (miesiąc pliku starszy niż "from").
    long fm = fileMonthTs(name);
    if (fm == 0) continue;
    if (fm < monthStartTs(from)) continue;

    File f = sdCard.openRead(fi.path);
    if (!f) continue;

    char buf[512];
    String line;
    line.reserve(256);
    bool header = true;
    size_t total = 0;
    int lines = 0;
    // Limit jak w module analizy (analysis.cpp) - starsze fragmenty bardzo
    // dużych plików są pomijane, żeby skan nie blokował serwera www.
    while (f.available() && total < 8 * 1024 * 1024) {
      int n = f.read((uint8_t*)buf, sizeof(buf));
      if (n <= 0) break;
      total += (size_t)n;
      for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
          if (line.length() && line[line.length() - 1] == '\r') line.remove(line.length() - 1);
          line.trim();
          if (line.length()) {
            if (!header) {
              int c1 = line.indexOf(',');
              if (c1 > 0) {
                long ts = atol(line.substring(0, c1).c_str());
                if (ts >= from && ts <= to) {
                  long b = bucketStart((time_t)ts, period);
                  int bi = -1;
                  auto it = bucketIdx.find(b);
                  if (it == bucketIdx.end()) {
                    bi = (int)out.size();
                    Bucket bk;
                    bk.ts = b;
                    out.push_back(bk);
                    bucketIdx[b] = bi;
                  } else {
                    bi = it->second;
                  }
                  // pola: timestamp(0), datetime(1), kolumny(2+)
                  int fld = 0, start = 0, idx = c1 + 1;
                  while (idx <= (int)line.length()) {
                    if (idx == (int)line.length() || line[idx] == ',') {
                      if (fld >= 2) {
                        int col = fld - 2;
                        auto cm = colMap.find(col);
                        if (cm != colMap.end() && idx > start) {
                          float v = atof(line.substring(start, idx).c_str());
                          if (!isnan(v)) out[bi].m[cm->second].add(v);
                        }
                      }
                      fld++;
                      start = idx + 1;
                    }
                    idx++;
                  }
                }
              }
            }
          }
          line = "";
          header = false;
          if ((++lines & 0x3F) == 0) { yield(); esp_task_wdt_reset(); }
        } else {
          line += c;
        }
      }
    }
    f.close();
  }

  return !out.empty();
}

static String fmt2(float v) {
  if (isnan(v)) return String("");
  char b[16];
  dtostrf(v, 0, 2, b);
  return String(b);
}

String ReportService::json(const String& period) {
  std::vector<Bucket> buckets;
  long from = 0, to = 0;
  if (!compute(period, buckets, from, to)) {
    return String("{\"error\":\"Brak danych dla tego okresu lub brak synchronizacji czasu\"}");
  }

  PsramAllocator alloc;
  JsonDocument d(&alloc);
  d["period"] = period;
  d["from"] = from;
  d["to"] = to;

  JsonArray metrics = d["metrics"].to<JsonArray>();
  for (int i = 0; i < NMETRICS; i++) {
    JsonObject m = metrics.add<JsonObject>();
    m["id"] = METRICS[i];
    m["name"] = metricName(METRICS[i]);
    m["unit"] = metricUnit(METRICS[i]);
  }

  JsonArray arr = d["buckets"].to<JsonArray>();
  for (auto& b : buckets) {
    JsonObject bo = arr.add<JsonObject>();
    bo["t"] = b.ts;
    bo["label"] = bucketLabel(b.ts, period);
    JsonObject v = bo["v"].to<JsonObject>();
    for (int i = 0; i < NMETRICS; i++) {
      if (b.m[i].n == 0) continue;
      JsonObject mv = v[METRICS[i]].to<JsonObject>();
      mv["min"] = b.m[i].mn;
      mv["avg"] = b.m[i].avg();
      mv["max"] = b.m[i].mx;
      mv["n"] = b.m[i].n;
    }
  }

  String s;
  serializeJson(d, s);
  return s;
}

String ReportService::csv(const String& period) {
  std::vector<Bucket> buckets;
  long from = 0, to = 0;
  if (!compute(period, buckets, from, to)) return String("");

  String out;
  out.reserve(4096);
  out += "data";
  for (int i = 0; i < NMETRICS; i++) {
    out += ",";
    out += METRICS[i];
    out += "_min,";
    out += METRICS[i];
    out += "_avg,";
    out += METRICS[i];
    out += "_max";
  }
  out += "\n";

  for (auto& b : buckets) {
    out += bucketLabel(b.ts, period);
    for (int i = 0; i < NMETRICS; i++) {
      out += ",";
      out += fmt2(b.m[i].mn);
      out += ",";
      out += fmt2(b.m[i].avg());
      out += ",";
      out += fmt2(b.m[i].mx);
    }
    out += "\n";
  }
  return out;
}
