/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
// Analiza danych pogodowych: rekordy (ekstrema), statystyki dzienne i trend
// ciśnienia. Moduł dokłada do danych już zapisanych w CSV na karcie SD warstwę
// "pogodową": minimum/maksimum każdego kanału, suma opadów dobowych oraz
// barometr (zmiana ciśnienia 3 h / 24 h).
//
// Rekordy są utrzymywane w pamięci i aktualizowane na bieżąco (noteSamples),
// a po starcie jednorazowo dosiane z istniejących plików CSV (loop -> seedNext).
// Statystyki dnia i trend ciśnienia liczone są na żądanie bezpośrednio z CSV.
#pragma once
#include <Arduino.h>
#include <vector>
#include "sensors.h"

class Analysis {
public:
  void begin();
  // Co interwał logowania: zaktualizuj rekordy wartościami z odczytów.
  void noteSamples(const std::vector<Channel>& channels);
  // Dosiewanie rekordów z CSV - wołane cyklicznie z loop() (jeden plik na raz).
  void loop();

  bool seeded() { return seeded_; }
  // Pełna analiza w JSON. Dane liczone są w tle (loop) i trzymane w pamięci
  // podręcznej, żeby żądanie HTTP nie blokowało karty SD i nie wyzwalało WDT.
  String json();
  // Rejestracja pojedynczej wartości (używana też przez skaner CSV).
  void noteValue(time_t ts, const String& metric, float v);

  // Róża wiatrów (kierunek + prędkość) liczona z CSV dla zadanego okresu.
  // period: "day" | "week" | "month" | "year". Zwraca gotowy JSON.
  String windRoseJson(const String& period);

  // Indeks jakości powietrza (CAQI) z PM2.5/PM10 + statystyki dnia z CSV.
  // Zwraca gotowy JSON.
  String aqiJson();

private:
  void buildCache();
  bool seedNext();
  void recordsJson(String& out);
  void todayJson(String& out);
  void pressureTrendJson(String& out);

  SemaphoreHandle_t mutex_ = nullptr;
  bool seeded_ = false;

  // Rekordy: metric -> min/max (+ timestamp).
  struct MetricRec {
    float mn = NAN;  time_t mnTs = 0;
    float mx = NAN;  time_t mxTs = 0;
  };
  std::vector<String> metricOrder_;          // kolejność pierwszej rejestracji
  std::vector<MetricRec> metricRec_;

  // Suma opadów dziennie (dayStart -> mm) - do rekordu "maksymalny opad dobowy".
  std::vector<long>   rainDayTs_;
  std::vector<float>  rainDayMm_;

  // Dosiewanie: lista plików i indeks postępu.
  std::vector<String> seedFiles_;
  int seedIdx_ = 0;
  unsigned long lastSeedMs_ = 0;

  // Pamięć podręczna wyniku JSON (budowana w tle, nie w żądaniu HTTP).
  String cache_;
  bool cacheValid_ = false;
  unsigned long lastBuildMs_ = 0;
};

extern Analysis analysis;
