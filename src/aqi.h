/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// Wynik obliczenia indeksu jakości powietrza (europejski CAQI) z PM2.5 / PM10.
struct AqiResult {
  bool available = false;   // false = brak jakichkolwiek danych PM
  float pm25 = NAN;         // wartość wejściowa (µg/m³) albo NAN
  float pm10 = NAN;         // wartość wejściowa (µg/m³) albo NAN
  int index = 0;            // 0..(>100) - im wyższy, tym gorzej
  const char* category = ""; // polska nazwa przedziału
  const char* color = "";    // kolor #rrggbb dla wstęgi / kafelka
  const char* advice = "";   // krótkie zalecenie po polsku
};

// Europejski CAQI: piecewise-linear z PM2.5 i PM10. Indeks to max z obu.
AqiResult aqiFromPm(float pm25, float pm10);

// Klasyfikacja CO2 wewnątrz (ppm) - zwraca krótką nazwę po polsku.
const char* co2Quality(float ppm);
// Kolor pasujący do klasyfikacji CO2.
const char* co2QualityColor(float ppm);
