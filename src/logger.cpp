/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "logger.h"
#include "config.h"
#include "sd_card.h"
#include "syslog.h"
#include <time.h>
#include <map>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <esp_task_wdt.h>

DataLogger logger;

// Kolejność kolumn w pliku CSV
static const char* COLS[] = {
  "temp", "hum", "press", "light",
  "ds0", "ds1", "ds2", "ds3", "ds4", "ds5", "ds6", "ds7",
  "rain", "wind", "vane", "pm1", "pm25", "pm10",
  // Czujniki jakości powietrza wewnątrz (SCD4x / SGP30)
  "co2", "eco2", "tvoc",
  // Dodatkowe czujniki (dopisywane na końcu, żeby stare pliki CSV
  // z karty SD czytały się dalej - patrz columnForMetric)
  "sht_t", "sht_h", "bmp_t", "bmp_p", "uv", "solar_wm2", "soil", "mmc_hdg",
  "in_pm1", "in_pm25", "in_pm4", "in_pm10",
  "sen_t", "sen_h", "sen_voc", "sen_nox",
  "ads0", "ads1", "ads2", "ads3",
  // Kanały z drugiego ESP (magistrala RS485) - prefiks "x2_". Dopisane na
  // samym końcu, żeby pliki CSV zapisane wcześniej czytały się bez zmian.
  "x2_temp", "x2_hum", "x2_press", "x2_light", "x2_rain", "x2_wind", "x2_vane",
  "x2_pm1", "x2_pm25", "x2_pm10", "x2_co2", "x2_eco2", "x2_tvoc",
  "x2_uv", "x2_solar_wm2", "x2_soil", "x2_mmc_hdg",
  "x2_in_pm1", "x2_in_pm25", "x2_in_pm4", "x2_in_pm10",
  "x2_sen_t", "x2_sen_h", "x2_sen_voc", "x2_sen_nox",
  "x2_sht_t", "x2_sht_h", "x2_bmp_t", "x2_bmp_p",
  "x2_ads0", "x2_ads1", "x2_ads2", "x2_ads3",
  "x2_ds0", "x2_ds1", "x2_ds2", "x2_ds3", "x2_ds4", "x2_ds5", "x2_ds6", "x2_ds7",
  // Druga temperatura na masterze (BME280, obok Tuya na "temp")
  "temp2"
};
static const int NCOLS = sizeof(COLS) / sizeof(COLS[0]);

static int colIndexOf(const String& col) {
  for (int i = 0; i < NCOLS; i++) if (col == COLS[i]) return i;
  return -1;
}

String DataLogger::csvHeader() {
  String h = "timestamp,datetime";
  for (int i = 0; i < NCOLS; i++) { h += ","; h += COLS[i]; }
  h += "\n";
  return h;
}

int DataLogger::columnCount() { return NCOLS; }

String DataLogger::columnName(int idx) {
  if (idx < 0 || idx >= NCOLS) return String("");
  return COLS[idx];
}

String DataLogger::columnForMetric(const String& metric) {
  if (metric == "temp") return "temp";
  if (metric == "temp2") return "temp2";
  if (metric == "hum") return "hum";
  if (metric == "press") return "press";
  if (metric == "light") return "light";
  if (metric.startsWith("ds_")) return String("ds") + metric.substring(3);
  if (metric == "rain") return "rain";
  if (metric == "wind") return "wind";
  if (metric == "vane") return "vane";
  if (metric == "pm1") return "pm1";
  if (metric == "pm25") return "pm25";
  if (metric == "pm10") return "pm10";
  if (metric == "co2") return "co2";
  if (metric == "eco2") return "eco2";
  if (metric == "tvoc") return "tvoc";
  // Formaldehyd (zewnętrzna stacja jakości powietrza) - nie ma własnej kolumny
  // w szerokim CSV, więc trafia do dziennika długiego /logs/extra-YYYY-MM.csv
  // (patrz logNow) i stamtąd czytają go wykresy.
  if (metric == "ch2o") return "ch2o";
  // Analiza zdjęcia z kamery (pogoda, faza dnia, zachmurzenie, deszcz, śnieg,
  // mgła, światło) - również bez kolumn w szerokim CSV, czyli dziennik długi.
  if (metric.startsWith("cam_")) return metric;
  // Stacja VEVOR (sniffer 868 MHz): temperatura, wilgotność, wiatr, porywy,
  // kierunek, deszcz (łącznie/dziś/tydzień/miesiąc/teraz), UV i światło
  // zewnętrzne. Bez kolumn w szerokim CSV - trafiają do dziennika długiego
  // /logs/extra-YYYY-MM.csv, z którego da się policzyć statystyki miesięczne
  // bez ruszania szerokiego pliku.
  if (metric.startsWith("vev_")) return metric;
  // Dodatkowe czujniki
  if (metric == "sht_t") return "sht_t";
  if (metric == "sht_h") return "sht_h";
  if (metric == "bmp_t") return "bmp_t";
  if (metric == "bmp_p") return "bmp_p";
  if (metric == "uv") return "uv";
  if (metric == "solar_wm2") return "solar_wm2";
  if (metric == "soil") return "soil";
  if (metric == "mmc_hdg") return "mmc_hdg";
  if (metric == "in_pm1") return "in_pm1";
  if (metric == "in_pm25") return "in_pm25";
  if (metric == "in_pm4") return "in_pm4";
  if (metric == "in_pm10") return "in_pm10";
  if (metric == "sen_t") return "sen_t";
  if (metric == "sen_h") return "sen_h";
  if (metric == "sen_voc") return "sen_voc";
  if (metric == "sen_nox") return "sen_nox";
  // Kanały przejmowane ze stacji jakości powietrza (in_iaq, in_mics_*, in_mq7_*,
  // in_air_*, in_pm25_1m/5m/24h, in_rh_corr, in_motion) - nie mają kolumn
  // w szerokim CSV, więc trafiają do dziennika długiego
  // /logs/extra-YYYY-MM.csv i stamtąd czytają je wykresy. Ta reguła MUSI być
  // poniżej jawnych zwrotów dla in_pm1/in_pm25/in_pm4/in_pm10, które mają
  // własne kolumny szerokiego pliku.
  if (metric.startsWith("in_")) return metric;
  if (metric.startsWith("ads_")) return String("ads") + metric.substring(4);
  // Kanały z węzłów RS485: id "x2_temp" -> kolumna "x2_temp",
  // "x2_ds_3" -> "x2_ds3", "x2_ads_2" -> "x2_ads2". Lista kolumn szerokiego
  // pliku ma tylko kanały węzła #2 (dopisane historycznie), więc kanały
  // węzłów #3 i dalszych zwracamy jako ich własne id - nie ma dla nich
  // kolumny, więc trafią do dziennika długiego /logs/extra-YYYY-MM.csv,
  // a wykresy czytają je stamtąd (getSeries -> seriesFromLong).
  if (SensorManager::isRemoteId(metric)) {
    int us = metric.indexOf('_');
    String pre = metric.substring(0, us + 1);      // "x2_", "x12_"
    String rest = metric.substring(us + 1);
    if (rest.startsWith("ds_"))  return pre + "ds" + rest.substring(3);
    if (rest.startsWith("ads_")) return pre + "ads" + rest.substring(4);
    return metric;
  }
  return "";
}

bool DataLogger::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();   // begin() bywa wołany ponownie
  bool ok = ensureLogsDir();
  if (ok) LOG_I("Zapis danych na kartę SD gotowy (/logs), interwał %u s",
                (unsigned)config.logIntervalS());
  else    LOG_W("Zapis danych na kartę SD wyłączony - brak karty");
  return ok;
}

bool DataLogger::ensureLogsDir() {
  if (!sdCard.mounted()) return false;
  // Bez tego /logs powstawałby dopiero przy pierwszym logu (a właściwie
  // wcale, bo SD.open() nie tworzy katalogów) - patrz SdCardManager::mkdirs.
  return sdCard.mkdirs("/logs");
}

bool DataLogger::logLine(const String& path, const String& line) {
  if (!sdCard.mounted()) return false;
  return sdCard.appendFile(path, line);
}

void DataLogger::logNow(const std::vector<Channel>& channels) {
  if (!sdCard.mounted()) return;

  time_t now = time(nullptr);
  if (now < 1000000000) return; // brak synchronizacji czasu

  struct tm tmv;
  localtime_r(&now, &tmv);
  char pathBuf[24];
  snprintf(pathBuf, sizeof(pathBuf), "/logs/%04d-%02d.csv", tmv.tm_year + 1900, tmv.tm_mon + 1);
  String path(pathBuf);

  bool needHeader = !sdCard.exists(path);
  if (needHeader) logLine(path, csvHeader());

  char tsBuf[16];
  snprintf(tsBuf, sizeof(tsBuf), "%ld", (long)now);

  char dtBuf[32];
  snprintf(dtBuf, sizeof(dtBuf), "%04d-%02d-%02d %02d:%02d:%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

  String tsLine = String(tsBuf) + "," + dtBuf;
  String line = tsLine;

  // Kanały bez własnej kolumny (sterowniki modułowe, np. vl53l0x_dist,
  // mpu_a_x) zapisujemy do dziennika długiego /logs/extra-YYYY-MM.csv jako
  // "timestamp,datetime,id,value" - dzięki temu lista czujników może rosnąć
  // bez rozszerzania nagłówka szerokiego CSV.
  char extraBuf[32];
  snprintf(extraBuf, sizeof(extraBuf), "/logs/extra-%04d-%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1);
  String extraPath(extraBuf);
  String extraRows;

  // Mapowanie id kanału -> kolumna
  bool logRemote = config.rs485Log();   // kanały drugiego ESP można wyłączyć w CSV
  String vals[NCOLS];
  for (const auto& c : channels) {
    if (c.remote && !logRemote) continue;
    if (isnan(c.value) || !c.enabled) continue;
    String col = columnForMetric(c.id);
    if (col.length() == 0) continue;
    char b[16];
    dtostrf(c.value, 0, 2, b);
    int ci = colIndexOf(col);
    if (ci >= 0) {
      vals[ci] = b;
      continue;
    }
    extraRows += tsLine;
    extraRows += ",";
    extraRows += c.id;
    extraRows += ",";
    extraRows += b;
    extraRows += "\n";
  }

  for (int i = 0; i < NCOLS; i++) {
    line += ",";
    line += vals[i].length() ? vals[i] : "";
  }
  line += "\n";

  if (extraRows.length()) {
    if (!sdCard.exists(extraPath)) logLine(extraPath, "timestamp,datetime,id,value\n");
    logLine(extraPath, extraRows);
  }

  // Błąd zapisu zgłaszamy raz, żeby nie zapełnić bufora logów w pętli.
  static bool reported = false;
  if (!logLine(path, line)) {
    if (!reported) {
      reported = true;
      LOG_E("Nie udało się zapisać pliku %s na karcie SD", path.c_str());
    }
  } else {
    reported = false;
  }
}

// ------------------------------------------------------------
//  Agregacja danych do wykresów
// ------------------------------------------------------------
struct Aggregate { double sum = 0, mn = 1e30, mx = -1e30; int n = 0; };

static long bucketStart(time_t ts, const String& period) {
  struct tm tmv;
  localtime_r(&ts, &tmv);
  struct tm b = tmv;
  b.tm_min = 0; b.tm_sec = 0;

  if (period == "day") {
    // przedziały godzinowe
  } else if (period == "week" || period == "month") {
    b.tm_hour = 0;
  } else if (period == "year") {
    b.tm_hour = 0; b.tm_mday = 1; b.tm_mon = 0;
  }
  return (long)mktime(&b);
}

static bool fileInRange(const String& name, long from, long to, const String& period) {
  // name = "YYYY-MM.csv" - plik obejmuje cały miesiąc, więc czytamy go tylko
  // wtedy, gdy jego zakres [początek miesiąca, początek następnego) przecina
  // żądane okno. Poprzednia wersja zwracała praktycznie zawsze true (zapas
  // 32 dni), więc wykres dzienny czytał także pliki sprzed miesiąca - przy
  // 1 MB CSV to kilka sekund blokady pętli głównej na każde żądanie.
  (void)period;
  int y, m;
  if (sscanf(name.c_str(), "%d-%d", &y, &m) != 2) return false;
  if (m < 1 || m > 12) return false;
  struct tm tmv = {};
  tmv.tm_year = y - 1900;
  tmv.tm_mon  = m - 1;
  tmv.tm_mday = 1;
  time_t fileStart = mktime(&tmv);
  tmv.tm_mon = m;              // m == 12 -> mktime sam przechodzi na styczeń
  time_t fileEnd = mktime(&tmv);
  return fileStart <= to && fileEnd > from;
}

// ------------------------------------------------------------
//  Odczyt dziennika długiego /logs/extra-YYYY-MM.csv
//  Plik ma wiersze "timestamp,datetime,id,value" - jedno pole na kanał.
// ------------------------------------------------------------
static bool seriesFromLong(const String& metric, long from, time_t now,
                           const String& period, std::vector<SeriesPoint>& out,
                           String& err) {
  std::vector<SdFileInfo> files;
  sdCard.listDir("/logs", files);

  std::map<long, Aggregate> buckets;

  // Parsuje jedną linię dziennika długiego "timestamp,datetime,id,value"
  // i dodaje wartość do koszyków, gdy kanał pasuje i linia mieści się w oknie.
  auto parseLongLine = [&](const char* line) {
    const char* c1 = strchr(line, ',');
    if (!c1) return;
    long ts = atol(line);
    if (ts < from || ts > now) return;
    const char* c2 = strchr(c1 + 1, ',');   // koniec daty/godziny
    if (!c2) return;
    const char* c3 = strchr(c2 + 1, ',');   // koniec id kanału
    if (!c3) return;
    // Porównanie dokładne - metric nie może być prefiksem innego id.
    if (c3 - (c2 + 1) != (int)metric.length()) return;
    if (strncmp(c2 + 1, metric.c_str(), (size_t)(c3 - (c2 + 1))) != 0) return;

    float v = atof(c3 + 1);
    if (!isfinite(v)) return;
    long b = bucketStart(ts, period);
    Aggregate& a = buckets[b];
    a.sum += v; a.n++;
    if (v < a.mn) a.mn = v;
    if (v > a.mx) a.mx = v;
  };

  for (auto& fi : files) {
    String name = fi.path;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.startsWith("extra-") || !name.endsWith(".csv")) continue;
    // Ten sam filtr miesięcy co dla szerokiego CSV - bez niego dziennik długi
    // czytał pliki spoza żądanego okresu.
    if (!fileInRange(name.substring(6), from, now, period)) continue;

    // Strumieniowe czytanie pliku (bez wczytywania całości do RAM-u). Plik
    // extra-*.csv rośnie ~1,5 MB dziennie, a wcześniejsza wersja czytała go
    // w całości do Stringu - przy 15,8 MB blokowała zadanie www > 5 s i
    // kończyła się resetem "watchdog zadania".
    File f = sdCard.openRead(fi.path);
    if (!f) continue;

    char buf[512];
    char line[256];
    size_t lineLen = 0;
    bool header = true;
    int lines = 0;
    while (f.available()) {
      int n = f.read((uint8_t*)buf, sizeof(buf));
      if (n <= 0) break;
      for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
          if (lineLen && line[lineLen - 1] == '\r') lineLen--;
          if (lineLen) {
            line[lineLen] = 0;
            if (header) header = false;
            else parseLongLine(line);
          }
          lineLen = 0;
          if ((++lines & 0x3F) == 0) { yield(); esp_task_wdt_reset(); }
        } else if (lineLen < sizeof(line) - 1) {
          line[lineLen++] = c;
        }
      }
    }
    // ostatnia linia bez znaku nowej linii
    if (lineLen) {
      if (line[lineLen - 1] == '\r') lineLen--;
      if (lineLen) {
        line[lineLen] = 0;
        if (!header) parseLongLine(line);
      }
    }
    f.close();
  }

  if (buckets.empty()) { err = "Brak danych dla tego okresu"; return false; }

  for (auto& kv : buckets) {
    SeriesPoint p;
    p.ts = kv.first;
    p.avg = (float)(kv.second.sum / kv.second.n);
    p.min = (float)kv.second.mn;
    p.max = (float)kv.second.mx;
    p.count = kv.second.n;
    out.push_back(p);
  }
  return true;
}

bool DataLogger::getSeries(const String& period, const String& metric,
                           std::vector<SeriesPoint>& out, String& err) {
  if (!sdCard.mounted()) { err = "Brak karty SD"; return false; }

  String col = columnForMetric(metric);
  if (col.length() == 0) { err = "Nieznany wskaźnik: " + metric; return false; }

  int colIdx = colIndexOf(col);

  time_t now = time(nullptr);
  if (now < 1000000000) { err = "Brak synchronizacji czasu"; return false; }

  long from = 0;
  if (period == "day")   from = now - 86400;
  else if (period == "week")  from = now - 7 * 86400;
  else if (period == "month") from = now - 30 * 86400;
  else if (period == "year")  from = now - 365 * 86400;
  else { err = "Zły okres: " + period; return false; }

  // Kanał bez własnej kolumny szerokiego CSV (sterowniki modułowe) - dane
  // są w dzienniku długim, patrz logNow().
  if (colIdx < 0) return seriesFromLong(metric, from, now, period, out, err);

  std::vector<SdFileInfo> files;
  sdCard.listDir("/logs", files);

  std::map<long, Aggregate> buckets;

  for (auto& fi : files) {
    String name = fi.path;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".csv") || name.startsWith(".")) continue;
    if (!fileInRange(name, from, now, period)) continue;

    // Strumieniowe czytanie pliku (bez wczytywania całości do RAM-u i bez
    // doklejania String znak po znaku - poprzednia wersja potrafiła blokować
    // pętlę główną na kilkanaście sekund przy miesięcznym pliku).
    File f = sdCard.openRead(fi.path);
    if (!f) continue;

    char buf[512];
    char line[2048];
    size_t lineLen = 0;
    bool header = true;
    int lines = 0;
    while (f.available()) {
      int n = f.read((uint8_t*)buf, sizeof(buf));
      if (n <= 0) break;
      for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
          if (lineLen && line[lineLen - 1] == '\r') lineLen--;
          while (lineLen && (line[lineLen - 1] == ' ' || line[lineLen - 1] == '\t')) lineLen--;
          if (lineLen) {
            line[lineLen] = 0;
            if (header) {
              header = false;
            } else {
              const char* c1 = strchr(line, ',');
              if (c1) {
                long ts = atol(line);
                if (ts >= from && ts <= now) {
                  const char* p = strchr(c1 + 1, ',');  // pomiń datetime
                  if (p) {
                    p++;
                    int field = 0;
                    while (*p) {
                      const char* comma = strchr(p, ',');
                      if (field == colIdx) {
                        if (comma ? comma > p : (*p != 0)) {
                          float v = atof(p);
                          if (!isnan(v)) {
                            long b = bucketStart(ts, period);
                            Aggregate& a = buckets[b];
                            a.sum += v; a.n++;
                            if (v < a.mn) a.mn = v;
                            if (v > a.mx) a.mx = v;
                          }
                        }
                        break;
                      }
                      if (!comma) break;
                      p = comma + 1;
                      field++;
                    }
                  }
                }
              }
            }
          }
          lineLen = 0;
          if ((++lines & 0x3F) == 0) { yield(); esp_task_wdt_reset(); }
        } else if (lineLen < sizeof(line) - 1) {
          line[lineLen++] = c;
        }
      }
    }
    // ostatnia linia bez znaku nowej linii
    if (lineLen) {
      if (line[lineLen - 1] == '\r') lineLen--;
      while (lineLen && (line[lineLen - 1] == ' ' || line[lineLen - 1] == '\t')) lineLen--;
      if (lineLen) {
        line[lineLen] = 0;
        if (!header) {
          const char* c1 = strchr(line, ',');
          if (c1) {
            long ts = atol(line);
            if (ts >= from && ts <= now) {
              const char* p = strchr(c1 + 1, ',');
              if (p) {
                p++;
                int field = 0;
                while (*p) {
                  const char* comma = strchr(p, ',');
                  if (field == colIdx) {
                    if (comma ? comma > p : (*p != 0)) {
                      float v = atof(p);
                      if (!isnan(v)) {
                        long b = bucketStart(ts, period);
                        Aggregate& a = buckets[b];
                        a.sum += v; a.n++;
                        if (v < a.mn) a.mn = v;
                        if (v > a.mx) a.mx = v;
                      }
                    }
                    break;
                  }
                  if (!comma) break;
                  p = comma + 1;
                  field++;
                }
              }
            }
          }
        }
      }
    }
    f.close();
  }

  if (buckets.empty()) { err = "Brak danych dla tego okresu"; return false; }

  for (auto& kv : buckets) {
    SeriesPoint p;
    p.ts = kv.first;
    p.avg = (float)(kv.second.sum / kv.second.n);
    p.min = (float)kv.second.mn;
    p.max = (float)kv.second.mx;
    p.count = kv.second.n;
    out.push_back(p);
  }
  return true;
}
