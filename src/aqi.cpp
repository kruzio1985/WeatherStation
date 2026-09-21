/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "aqi.h"
#include <math.h>

// Interpolacja liniowa CAQI między punktami (wartość -> indeks).
static float caqiLerp(float v, const float* vals, const float* idxs, int n) {
  if (isnan(v) || v < 0) return 0.0f;
  if (v <= vals[0]) return idxs[0];
  for (int i = 1; i < n; i++) {
    if (v <= vals[i]) {
      float t = (v - vals[i - 1]) / (vals[i] - vals[i - 1]);
      return idxs[i - 1] + t * (idxs[i] - idxs[i - 1]);
    }
  }
  // Powyżej górnej granicy - ekstrapolacja ostatnim odcinkiem.
  float t = (v - vals[n - 2]) / (vals[n - 1] - vals[n - 2]);
  return idxs[n - 2] + t * (idxs[n - 1] - idxs[n - 2]);
}

static void aqiBand(int index, const char** cat, const char** col, const char** adv) {
  if (index <= 25) {
    *cat = "Bardzo dobry";
    *col = "#4dabf7";
    *adv = "Powietrze bardzo dobre - idealne warunki do aktywności na zewnątrz.";
  } else if (index <= 50) {
    *cat = "Dobry";
    *col = "#69db7c";
    *adv = "Powietrze dobre - aktywność na zewnątrz bez ograniczeń.";
  } else if (index <= 75) {
    *cat = "Umiarkowany";
    *col = "#ffd43b";
    *adv = "Powietrze umiarkowane - osoby wrażliwe powinny ograniczyć długi wysiłek na zewnątrz.";
  } else if (index <= 100) {
    *cat = "Dostateczny";
    *col = "#ff922b";
    *adv = "Powietrze dostateczne - rozważ ograniczenie wysiłku na zewnątrz.";
  } else {
    *cat = "Zły";
    *col = "#ff6b6b";
    *adv = "Powietrze złe - unikaj długotrwałego wysiłku na zewnątrz.";
  }
}

AqiResult aqiFromPm(float pm25, float pm10) {
  AqiResult r;
  bool has25 = !isnan(pm25) && pm25 >= 0;
  bool has10 = !isnan(pm10) && pm10 >= 0;
  if (!has25 && !has10) return r;  // available = false

  r.available = true;
  if (has25) r.pm25 = pm25;
  if (has10) r.pm10 = pm10;

  static const float V25[] = { 0, 15, 30, 55, 110 };
  static const float I25[] = { 0, 25, 50, 75, 100 };
  static const float V10[] = { 0, 25, 50, 90, 180 };
  static const float I10[] = { 0, 25, 50, 75, 100 };

  float i25 = has25 ? caqiLerp(pm25, V25, I25, 5) : 0.0f;
  float i10 = has10 ? caqiLerp(pm10, V10, I10, 5) : 0.0f;
  r.index = (int)lroundf(i25 > i10 ? i25 : i10);
  aqiBand(r.index, &r.category, &r.color, &r.advice);
  return r;
}

const char* co2Quality(float ppm) {
  if (isnan(ppm)) return "";
  if (ppm < 600) return "Bardzo dobre";
  if (ppm < 1000) return "Dobre";
  if (ppm < 1500) return "Umiarkowane";
  if (ppm < 2000) return "Złe";
  return "Bardzo złe";
}

const char* co2QualityColor(float ppm) {
  if (isnan(ppm)) return "#888888";
  if (ppm < 600) return "#69db7c";
  if (ppm < 1000) return "#a9e34b";
  if (ppm < 1500) return "#ffd43b";
  if (ppm < 2000) return "#ff922b";
  return "#ff6b6b";
}
