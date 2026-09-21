/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <vector>
#include "sensors.h"

// Pojedynczy punkt wykresu (zagregowany)
struct SeriesPoint {
  long ts;      // timestamp unix (początek przedziału)
  float avg;
  float min;
  float max;
  int count;
};

class DataLogger {
public:
  bool begin();
  void logNow(const std::vector<Channel>& channels);

  // period: "day" | "week" | "month" | "year"
  // metric: id kanału (temp, hum, press, ...)
  bool getSeries(const String& period, const String& metric,
                 std::vector<SeriesPoint>& out, String& err);

  String columnForMetric(const String& metric);
  static String csvHeader();
  // Liczba i nazwa kolumn szerokiego pliku CSV (do skanowania przez moduł
  // analizy - rekordy, statystyki dzienne, trend ciśnienia).
  static int columnCount();
  static String columnName(int idx);

private:
  bool logLine(const String& path, const String& line);
  bool ensureLogsDir();

  SemaphoreHandle_t mutex_ = nullptr;
};

extern DataLogger logger;
