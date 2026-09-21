/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Prognoza Open-Meteo (7 dni).
//
//  Pobiera codzienną prognozę z https://api.open-meteo.com dla
//  współrzędnych skonfigurowanych w stacji (latitude/longitude).
//  Dane trzymane są w pamięci podręcznej (PSRAM przy parsowaniu JSON),
//  a strona www odczytuje je przez GET /api/openmeteo.
//
//  Ustawienia trzymane są w sekcji "extra" konfiguracji:
//    om_on  (1/0)  - prognoza włączona (domyślnie 1),
//    om_int (min)  - interwał pobierania (domyślnie 60).
// =============================================================

#define OM_DAYS 7

struct OmDay {
  char date[11];       // "YYYY-MM-DD"
  float tmin = NAN;
  float tmax = NAN;
  float precip = NAN;        // suma opadu, mm
  float precipProb = NAN;    // prawdopodobieństwo opadu, %
  float wind = NAN;          // maks. prędkość wiatru, km/h
  int   code = -1;           // kod pogody WMO
  char sunrise[6];           // "HH:MM"
  char sunset[6];
};

struct OmCurrent {
  bool  valid = false;
  float temp = NAN;
  float hum = NAN;
  float app = NAN;           // temperatura odczuwalna
  float wind = NAN;          // km/h
  int   code = -1;
  bool  isDay = true;
};

class OpenMeteoService {
public:
  void begin();

  // Natychmiastowe pobranie (przycisk "Odśwież teraz" na stronie www)
  void requestNow() { requestNow_ = true; }

  bool enabled();
  void setEnabled(bool on);
  uint32_t intervalMin();
  void setIntervalMin(uint32_t minutes);

  // Zapis ustawień z żądania POST /api/openmeteo ({"enabled":..,"interval_min":..})
  bool applyJson(const char* json, size_t len);

  // Stan prognozy i bufor danych dla strony www (JSON)
  String json();

private:
  static void taskEntry(void* arg);
  void run();
  bool fetch();

  SemaphoreHandle_t mutex_ = nullptr;
  OmDay days_[OM_DAYS];
  OmCurrent current_;
  int dayCount_ = 0;
  bool valid_ = false;          // czy bufor zawiera dane
  unsigned long fetchedAtMs_ = 0;
  String error_;
  volatile bool requestNow_ = false;
};

extern OpenMeteoService openMeteo;
