/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>

// =============================================================
//  Moduł GPS (odbiornik NMEA 0183 na UART, np. NEO-6M, ATGM336H,
//  L86, BN-880). Biblioteka nie jest potrzebna - ramki GGA/RMC/GSA
//  parsujemy sami, dzięki czemu firmware zostaje bez dodatkowych
//  zależności.
//
//  Poza pozycją GPS daje też dokładny czas UTC - używamy go do
//  korekty zegara, gdy w sieci nie ma NTP (np. brak internetu).
// =============================================================

struct GpsFix {
  bool valid = false;         // ostatnia poprawna ramka zawierała pozycję
  double lat = 0.0;           // stopnie (ujemne = S)
  double lon = 0.0;           // stopnie (ujemne = W)
  float altitudeM = NAN;      // wysokość n.p.m.
  float geoidM = NAN;         // undulacja geoidy
  float speedKmh = NAN;       // prędkość
  float courseDeg = NAN;      // kurs (kierunek ruchu)
  float hdop = NAN;           // dokładność pozioma
  int sats = 0;               // liczba satelitów użytych w rozwiązaniu
  int satsView = 0;           // satelity widoczne (GSV)
  int fixQuality = 0;         // 0 = brak, 1 = GPS, 2 = DGPS, 4 = RTK
  int fixType = 0;            // 1 = brak, 2 = 2D, 3 = 3D (z GSA)
  bool timeValid = false;     // poprawna data i godzina UTC
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  unsigned long lastFixMs = 0;         // millis() ostatniej ramki z pozycją
  unsigned long lastSentenceMs = 0;    // millis() dowolnej poprawnej ramki
  unsigned long sentences = 0;         // liczba poprawnych ramek
  unsigned long errors = 0;            // liczba ramek z błędną sumą kontrolną
};

class GpsService {
public:
  // Otwiera port szeregowy, jeśli pin RX jest przypisany (zakładka "Piny").
  // Zwraca false, gdy GPS jest wyłączony (-1) lub port się nie otworzył.
  bool begin();

  // Zamyka port UART1 (na czas diagnostyki magistrali, która też go używa).
  // Po zakończeniu wystarczy ponownie wywołać begin().
  void end();

  // Wołane z loop() - opróżnia bufor UART i parsuje ramki.
  void loop();

  bool enabled() const { return enabled_; }
  bool present() const { return fix_.sentences > 0; }   // cokolwiek odebrano
  bool hasFix() const;                                  // świeża pozycja
  const GpsFix& raw() const { return fix_; }

  // Czas GPS jako epoka UTC (0 = brak poprawnego czasu)
  time_t utcEpoch() const;

  // Ile sekund minęło od ostatniej poprawnej ramki (999 = brak danych)
  unsigned long ageS() const;

  String json() const;          // GET /api/gps

private:
  void feed(char c);
  void parseSentence(char* s);
  bool parseCoord(const char* v, char hemi, double& out) const;
  bool parseTimeUtc(const char* hhmmss);
  bool parseDateDmy(const char* ddmmyy);
  void maybeSyncClock();

  bool enabled_ = false;
  int8_t rxPin_ = -1;
  int8_t txPin_ = -1;
  uint32_t baud_ = 9600;
  char buf_[128] = {0};
  size_t len_ = 0;
  GpsFix fix_;
  bool clockSynced_ = false;
  unsigned long lastSyncTryMs_ = 0;
};

extern GpsService gps;
