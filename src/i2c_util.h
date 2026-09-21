/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#pragma once
#include <Arduino.h>
#include <Wire.h>

// =============================================================
//  Wspólne pomocniki I2C dla sterowników dopisywanych modułowo
//  (SHT4x, BMP581, VEML7700, LTR-390UV, SEN5x, SPS30, AS3935...).
//
//  W sensors.cpp te same funkcje są statyczne, więc tutaj mamy
//  wersje inline o innych nazwach - dzięki temu każdy sterownik
//  może ich używać bez duplikowania kodu.
// =============================================================

// Odczyt N bajtów z rejestru/pod adresem
inline bool i2cuReadReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned long t = millis();
    while (!Wire.available() && millis() - t < 20) delay(1);
    if (!Wire.available()) return false;
    buf[i] = (uint8_t)Wire.read();
  }
  return true;
}

// Odczyt N bajtów bez wysyłania adresu rejestru (np. po komendzie 16-bit)
inline bool i2cuRead(uint8_t addr, uint8_t* buf, size_t len) {
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned long t = millis();
    while (!Wire.available() && millis() - t < 20) delay(1);
    if (!Wire.available()) return false;
    buf[i] = (uint8_t)Wire.read();
  }
  return true;
}

// Zapis jednego bajtu do rejestru
inline bool i2cuWriteReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Zapis pojedynczego bajtu-komendy (SHT4x, LTR-390UV, AS5600, MMC5983MA...)
inline bool i2cuWrite8(uint8_t addr, uint8_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write(cmd);
  return Wire.endTransmission() == 0;
}

// Zapis rejestru 16-bit + wartość 16-bit (SEN5x/SPS30 używają 16-bitowych komend)
inline bool i2cuWrite16(uint8_t addr, uint16_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

inline bool i2cuWrite16Arg(uint8_t addr, uint16_t cmd, uint16_t arg) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  Wire.write((uint8_t)(arg >> 8));
  Wire.write((uint8_t)(arg & 0xFF));
  return Wire.endTransmission() == 0;
}

inline bool i2cuPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// CRC-8 Sensirion/SPS (poly 0x31, init 0xFF)
inline uint8_t i2cuCrc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Sprawdzenie trzech bloków danych z CRC po każdym bajcie (format Sensirion)
inline bool i2cuCheckCrc3(const uint8_t* buf, size_t words) {
  for (size_t i = 0; i < words; i++) {
    if (i2cuCrc8(buf + i * 3, 2) != buf[i * 3 + 2]) return false;
  }
  return true;
}

// ---- IEEE754 half-float (VEML7700, LTR-390UV, BMP581 zapisują tak dane) ----
inline float i2cuHalfToFloat(uint16_t h) {
  uint32_t sign = (uint32_t)(h >> 15) & 0x1u;
  uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
  uint32_t mant = (uint32_t)(h & 0x3FFu);
  float v;
  if (exp == 0) {
    v = (mant == 0) ? 0.0f : ldexpf((float)mant, -24);
  } else if (exp == 31) {
    v = (mant == 0) ? 0.0f : NAN;
  } else {
    v = ldexpf((float)(mant | 0x400u), (int)exp - 25);
  }
  return sign ? -v : v;
}
