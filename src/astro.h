/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <time.h>

// =============================================================
//  Astronomia: pozycja Słońca i Księżyca, wschody/zachody,
//  zmierzch cywilny, długość dnia, faza Księżyca.
//
//  Algorytmy liczone są w firmware (bez zależności zewnętrznych):
//  Słońce - według uproszczonego algorytmu NOAA, Księżyc - według
//  uproszczonego modelu Meeusa. Dokładność: Słońce ok. ±1 min dla
//  wschodu/zachodu, Księżyc ok. ±5 min (wystarczające dla stacji).
//
//  Położenie bierzemy z GPS (gdy jest fix), a bez GPS z ustawień
//  stacji (szerokość/długość geograficzna wpisana na stronie www).
// =============================================================

struct SunInfo {
  bool valid = false;
  bool polarDay = false;      // dzień polarny (brak zachodu)
  bool polarNight = false;    // noc polarna (brak wschodu)
  float elevationDeg = NAN;   // wysokość nad horyzontem (°)
  float azimuthDeg = NAN;     // azymut (0 = N, 90 = E)
  float declinationDeg = NAN; // deklinacja Słońca
  float maxElevationDeg = NAN;
  float solarNoonMin = NAN;   // minuty od północy czasu lokalnego
  float sunriseMin = NAN;
  float sunsetMin = NAN;
  float civilDawnMin = NAN;
  float civilDuskMin = NAN;
  float dayLengthH = NAN;
};

struct MoonInfo {
  bool valid = false;
  float phase = 0.0f;         // 0 = nów, 0.5 = pełnia, 1 = nów
  float illumination = 0.0f;  // udział oświetlonej tarczy 0..1
  float ageDays = 0.0f;       // wiek Księżyca (doby od nowiu)
  float altitudeDeg = NAN;
  float azimuthDeg = NAN;
  float distanceKm = NAN;
  float riseMin = NAN;        // minuty od północy czasu lokalnego
  float setMin = NAN;
  const char* phaseKey = "new";  // klucz fazy do tłumaczenia na www
};

class AstroService {
public:
  // Przelicza Słońce i Księżyc dla aktualnego czasu i położenia.
  // lat/lon w stopniach (dodatnia = N/E), tzMin = przesunięcie strefy
  // czasowej w minutach względem UTC.
  void update(double lat, double lon, time_t utcNow, int tzMin);

  const SunInfo& sun() const { return sun_; }
  const MoonInfo& moon() const { return moon_; }
  bool valid() const { return valid_; }
  double latitude() const { return lat_; }
  double longitude() const { return lon_; }
  bool fromGps() const { return fromGps_; }

  String json() const;   // GET /api/astro

  // Surowe obliczenia dla dowolnego czasu (używane też przez prognozę)
  static void computeSun(double lat, double lon, time_t utc, int tzMin, SunInfo& out);
  static void computeMoon(double lat, double lon, time_t utc, int tzMin, MoonInfo& out);
  static int localTzOffsetMin();

private:
  SunInfo sun_;
  MoonInfo moon_;
  bool valid_ = false;
  bool fromGps_ = false;
  double lat_ = 0.0;      // szerokość geograficzna użyta do obliczeń
  double lon_ = 0.0;      // długość geograficzna użyta do obliczeń
};

extern AstroService astro;
