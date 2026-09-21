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
//  Dodatkowe czujniki I2C.
//
//  Każdy sterownik sam potwierdza, że układ naprawdę jest na
//  magistrali (WHO AM I / odpowiedź na komendę + CRC), więc kanał
//  uznany za "wykryty" znaczy wykryty sprzętowo, a nie sam pin.
//  Dzięki temu na stronie www nie ma fałszywych "połączony".
//
//  Sterowniki są napisane ręcznie (bez zewnętrznych bibliotek),
//  żeby nie zajmowały flasha i nie kolidowały z wersjami bibliotek
//  użytymi już na tej płytce.
// =============================================================

// ---------- SHT4x (SHT40 / SHT41 / SHT45) ----------
class Sht4x {
 public:
  bool begin();                       // wykrywa 0x44 / 0x45 / 0x46
  bool read(float& tempC, float& rh);
  bool ok() const { return ok_; }
  uint8_t addr() const { return addr_; }
 private:
  uint8_t addr_ = 0;
  bool ok_ = false;
};

// ---------- BMP581 / BMP580 (ciśnienie + temperatura) ----------
class Bmp581 {
 public:
  bool begin();                       // wykrywa 0x46 / 0x47
  bool read(float& tempC, float& pressHpa);
  bool ok() const { return ok_; }
  uint8_t addr() const { return addr_; }
 private:
  uint8_t addr_ = 0;
  bool ok_ = false;
};

// ---------- VEML7700 (natężenie światła, do ~15 klx) ----------
class Veml7700 {
 public:
  bool begin();
  bool read(float& lux);
  bool ok() const { return ok_; }
 private:
  bool ok_ = false;
};

// ---------- LTR-390UV (indeks UV) ----------
// Odczyt pracuje w trybie UVS (tryb wybiera się w rejestrze MAIN_CTRL,
// więc ALS i UVS nie da się czytać jednocześnie). Kanał ALS ma
// VEML7700/BH1750, dlatego tutaj liczy się tylko indeks UV.
class Ltr390uv {
 public:
  bool begin();
  bool read(float& uvIndex);
  bool ok() const { return ok_; }
 private:
  bool ok_ = false;
};

// ---------- MMC5983MA (kompas - azymut magnetyczny) ----------
class Mmc5983 {
 public:
  bool begin();
  bool readHeading(float& headingDeg);
  bool ok() const { return ok_; }
  uint8_t addr() const { return addr_; }
 private:
  uint8_t addr_ = 0;
  bool ok_ = false;
};

// ---------- AS5600 (magnetyczny enkoder kąta - wiatrowskaz) ----------
class As5600 {
 public:
  bool begin();
  bool readAngle(float& deg);         // 0..360
  bool magnetOk() const { return magnetOk_; }
  bool ok() const { return ok_; }
 private:
  bool ok_ = false;
  bool magnetOk_ = false;
};

// ---------- ADS1115 (4-kanalowy ADC 16 bit na I2C) ----------
// Używany dla sygnałów analogowych, których nie da się podłączyć do
// ADC1 ESP32-S3 (albo gdy potrzebna jest lepsza rozdzielczość).
class Ads1115 {
 public:
  bool begin();                                  // wykrywa 0x48..0x4B
  bool readChannel(uint8_t ch, float& volts);    // ch 0..3
  bool ok() const { return ok_; }
  uint8_t addr() const { return addr_; }
 private:
  uint8_t addr_ = 0;
  bool ok_ = false;
};
