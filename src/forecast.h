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
//  Prognoza lokalna i analiza pogody.
//
//  Wszystko liczone jest w firmware z danych czujników (bez chmury):
//    * punkt rosy, wilgotność bezwzględna, wskaźnik ciepła, temperatura
//      odczuwalna (wiatr), humidex,
//    * trend ciśnienia (1 h / 3 h / 6 h) z 24-godzinnej historii,
//    * prawdopodobieństwo deszczu, ryzyko burzy (AS3935 + spadek
//      ciśnienia + wiatr), ryzyko mgły (RH, T-Td, widzialność, brak
//      deszczu, wiatr),
//    * prognoza słowna w stylu Zambrettiego (ciśnienie + trend + wiatr)
//      wraz z pewnością prognozy i listą czynników ("szczegółowa analiza").
//
//  Historia (24 h, próbka co 5 min) trzymana jest w RAM (PSRAM nie jest
//  potrzebny - 288 próbek × kilka pól to ok. 7 kB).
// =============================================================

#define FC_HIST_MAX   288          // 24 h przy próbce co 5 min
#define FC_SAMPLE_MS  300000UL     // 5 min

struct FogInfo {
  float risk = 0.0f;          // 0..100
  bool  active = false;       // risk >= 70
  float spreadC = NAN;        // T - punkt rosy
  float visibilityKm = NAN;   // z czujnika widzialności (jeśli jest)
};

struct StormInfo {
  float risk = 0.0f;          // 0..100
  bool  nearby = false;       // wyładowanie bliżej niż 20 km w ciągu 15 min
  float lastDistanceKm = NAN;
  unsigned long strikes15m = 0;
  unsigned long strikes1h = 0;
  unsigned long strikesTotal = 0;
  float lastEnergy = NAN;
  unsigned long lastStrikeAgeS = 999;
};

struct ForecastInfo {
  bool valid = false;
  bool clockValid = false;

  // Wskaźniki meteorologiczne
  float dewPointC = NAN;
  float absHumidity = NAN;      // g/m³
  float heatIndexC = NAN;
  float windChillC = NAN;
  float humidexC = NAN;
  float cloudiness = NAN;       // 0..1 (z pyranometru/luksomierza, gdy są)
  float clearSkyWm2 = NAN;

  // Trend ciśnienia
  bool  trendValid = false;
  float trend1h = NAN;          // hPa/h
  float trend3h = NAN;          // hPa / 3 h
  const char* trendKey = "unknown";

  // Prognoza
  float rainProbability = NAN;  // 0..100
  const char* rainKey = "unknown";
  float  confidence = 0.0f;     // 0..1
  const char* outlookKey = "unknown";
  int    outlookLevel = 0;      // 0 = brak danych, wyżej = gorzej

  FogInfo fog;
  StormInfo storm;

  // Źródła danych (do diagnostyki i wykresów)
  bool hasTemp = false, hasHum = false, hasPress = false, hasWind = false, hasRain = false, hasLight = false;
  float tempC = NAN, humPct = NAN, pressHpa = NAN, windKmh = NAN;
  float rainMm1h = NAN, rainMm24h = NAN;
  float solarWm2 = NAN, uvIndex = NAN;
  float windDirDeg = NAN;
  const char* windDirKey = "";
  unsigned long lastUpdateMs = 0;
};

struct ForecastFactor {
  const char* key;      // klucz do tłumaczenia na www
  float value;          // wartość (może być NAN)
  int weight;           // wpływ na prognozę: -100..+100 (ujemny = pogorszenie)
  bool available;
};

class ForecastService {
public:
  void begin();
  void loop();             // zapisuje próbki historii (co 5 min)
  void update(time_t nowEpochUtc, int tzOffsetMin);  // przelicza wskaźniki

  const ForecastInfo& info() const { return info_; }

  String json() const;             // GET /api/forecast
  String analysisJson() const;     // lista czynników + opis słowny

  // Historia ciśnienia dla wykresu (zakładka "Wykresy"/"Prognoza")
  size_t historyCount() const { return histCount_; }
  time_t historyTime(size_t i) const;
  float  historyPressure(size_t i) const;
  float  historyTemp(size_t i) const;
  float  historyHum(size_t i) const;

  bool fogActive() const { return info_.fog.active; }
  bool stormNearby() const { return info_.storm.nearby; }
  float rainProbability() const { return info_.rainProbability; }
  float pressureTrend1h() const { return info_.trend1h; }

  // Zdarzenie z detektora wyładowań (wołane przez sterownik AS3935)
  void addLightning(float distanceKm, float energy);

private:
  struct Sample {
    time_t t = 0;
    unsigned long ms = 0;
    float press = NAN;
    float temp = NAN;
    float hum = NAN;
    float wind = NAN;
    float rain = NAN;
    float solar = NAN;
  };

  Sample hist_[FC_HIST_MAX];
  size_t histCount_ = 0;      // liczba próbek (do FC_HIST_MAX)
  size_t histHead_ = 0;       // następny indeks zapisu

  ForecastInfo info_;
  unsigned long lastSampleMs_ = 0;
  bool started_ = false;

  // Zdarzenia wyładowań (pierścień 32 zdarzeń)
  struct Strike { unsigned long ms; float dist; float energy; };
  Strike strikes_[32];
  size_t strikeHead_ = 0, strikeCount_ = 0;

  void pushSample();
  const Sample* oldest() const;
  const Sample* sampleAt(size_t i) const;
  float pressureAtAgeMinutes(int minutes) const;
  void computeDerived(time_t nowEpochUtc);
  void computeTrend();
  void computeFogAndStorm();
  void computeOutlook();
};

extern ForecastService forecast;
