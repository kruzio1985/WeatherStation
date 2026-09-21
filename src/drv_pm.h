/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include "i2c_util.h"

// =============================================================
//  Czujnik cząstek stałych (PM1.0 / PM2.5 / PM4.0 / PM10).
//
//  Dwa układy dzielą ten sam adres 0x69, więc rozpoznajemy je po
//  obsługiwanych komendach:
//    * Sensirion SEN5x (SEN50/SEN54/SEN55) - pyta o nazwę produktu
//      (0xD014) i dodatkowo podaje temperaturę, wilgotność oraz
//      indeksy VOC i NOx,
//    * Sensirion SPS30 - startuje pomiar (0x0010) i odsyła 4 liczby
//      zmiennoprzecinkowe (każde 16 bit + CRC).
//
//  Układ włącza się dopiero po starcie pomiaru - robimy to raz
//  w begin(), żeby nie zajmować magistrali co odczyt.
// =============================================================

enum PmKind : uint8_t {
  PM_NONE = 0,
  PM_SEN5X,
  PM_SPS30,
};

struct PmData {
  float pm1 = NAN;
  float pm25 = NAN;
  float pm4 = NAN;
  float pm10 = NAN;
  float tempC = NAN;    // tylko SEN5x
  float rh = NAN;       // tylko SEN5x
  float voc = NAN;      // tylko SEN5x (indeks 1..500)
  float nox = NAN;      // tylko SEN5x (indeks 1..500)
};

class PmAir {
 public:
  bool begin();                    // wykrywa SEN5x albo SPS30 pod 0x69
  bool read(PmData& d);            // odczyt bieżących wartości
  PmKind kind() const { return kind_; }
  const char* kindName() const;    // "SEN5x" / "SPS30" / ""
 private:
  bool beginSen5x();
  bool beginSps30();
  bool readSen5x(PmData& d);
  bool readSps30(PmData& d);

  PmKind kind_ = PM_NONE;
};
