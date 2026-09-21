/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_pm.h"

// =============================================================
//  Czujnik pyłu: Sensirion SEN5x albo SPS30 (oba pod adresem 0x69).
//
//  Rozpoznanie:
//    * SEN5x  - odpowiada nazwą produktu (0xD014, np. "SEN55"),
//    * SPS30  - odpowiada na komendę wersji oprogramowania (0xD100)
//               i poprawnym CRC; komendy 16-bitowe, argumenty i dane
//               zawsze z bajtem CRC (poly 0x31, init 0xFF).
//
//  Uwaga: SPS30 nie obsługuje "repeated start" - komendę i odczyt
//  wysyłamy jako dwie osobne transakcje (tak robi też sterownik
//  w Linuksie), co nasze pomocniki i2cu* robią domyślnie.
// =============================================================

#define PM_ADDR             0x69
#define PM_CMD_READ_MEAS    0x03C4   // SEN5x
#define PM_CMD_START_SEN5X  0x0021
#define PM_CMD_PRODUCT_NAME 0xD014
#define PM_SPS_CMD_START    0x0010   // SPS30, argument 0x0300 = float BE
#define PM_SPS_CMD_STOP     0x0104
#define PM_SPS_CMD_READ     0x0300
#define PM_SPS_CMD_VERSION  0xD100

const char* PmAir::kindName() const {
  switch (kind_) {
    case PM_SEN5X: return "SEN5x";
    case PM_SPS30: return "SPS30";
    default: return "";
  }
}

bool PmAir::begin() {
  if (!i2cuPresent(PM_ADDR)) {
    kind_ = PM_NONE;
    return false;
  }
  if (beginSen5x()) return true;
  if (beginSps30()) return true;
  kind_ = PM_NONE;
  return false;
}

// ---------- SEN5x ----------
bool PmAir::beginSen5x() {
  if (!i2cuWrite16(PM_ADDR, PM_CMD_PRODUCT_NAME)) return false;
  delay(20);

  uint8_t buf[48];   // 16 słów po 2 bajty + CRC
  if (!i2cuRead(PM_ADDR, buf, sizeof(buf))) return false;
  if (!i2cuCheckCrc3(buf, 16)) return false;

  char name[17];
  uint8_t n = 0;
  for (uint8_t i = 0; i < 16 && n < 16; i++) {
    char c = (char)buf[i * 3];
    if (c == 0 || (uint8_t)c == 0xFF) break;
    name[n++] = c;
  }
  name[n] = 0;
  if (strncmp(name, "SEN5", 4) != 0) return false;

  // Restart pomiaru, żeby układ pracował w znanym stanie (np. po resecie ESP32)
  i2cuWrite16(PM_ADDR, 0x3F86);   // stop measurement
  delay(60);
  if (!i2cuWrite16(PM_ADDR, PM_CMD_START_SEN5X)) return false;
  delay(60);

  kind_ = PM_SEN5X;
  return true;
}

bool PmAir::readSen5x(PmData& d) {
  if (!i2cuWrite16(PM_ADDR, PM_CMD_READ_MEAS)) return false;
  delay(20);

  uint8_t buf[24];   // 8 słów: 4x PM, wilgotność, temperatura, VOC, NOx
  if (!i2cuRead(PM_ADDR, buf, sizeof(buf))) return false;
  if (!i2cuCheckCrc3(buf, 8)) return false;

  uint16_t w[8];
  for (uint8_t i = 0; i < 8; i++) w[i] = (uint16_t)((buf[i * 3] << 8) | buf[i * 3 + 1]);

  // 0xFFFF = wartość jeszcze niegotowa
  if (w[0] != 0xFFFF) d.pm1 = w[0] / 10.0f;
  if (w[1] != 0xFFFF) d.pm25 = w[1] / 10.0f;
  if (w[2] != 0xFFFF) d.pm4 = w[2] / 10.0f;
  if (w[3] != 0xFFFF) d.pm10 = w[3] / 10.0f;
  if (w[4] != 0xFFFF) d.rh = (int16_t)w[4] / 100.0f;
  if (w[5] != 0xFFFF) d.tempC = (int16_t)w[5] / 200.0f;
  // Indeksy 1..500 - 0 i 0xFFFF oznaczają brak danych
  if (w[6] != 0xFFFF && w[6] != 0) d.voc = (int16_t)w[6] / 10.0f;
  if (w[7] != 0xFFFF && w[7] != 0) d.nox = (int16_t)w[7] / 10.0f;
  return true;
}

// ---------- SPS30 ----------
bool PmAir::beginSps30() {
  // Komenda wersji oprogramowania - nie zmienia stanu czujnika
  if (!i2cuWrite16(PM_ADDR, PM_SPS_CMD_VERSION)) return false;
  delay(20);
  uint8_t v[3];
  if (!i2cuRead(PM_ADDR, v, 3)) return false;
  if (i2cuCrc8(v, 2) != v[2]) return false;

  i2cuWrite16(PM_ADDR, PM_SPS_CMD_STOP);
  delay(20);

  // Start pomiaru: komenda + argument 0x0300 + CRC argumentu
  uint8_t arg[2] = {0x03, 0x00};
  uint8_t crc = i2cuCrc8(arg, 2);
  Wire.beginTransmission(PM_ADDR);
  Wire.write((uint8_t)(PM_SPS_CMD_START >> 8));
  Wire.write((uint8_t)(PM_SPS_CMD_START & 0xFF));
  Wire.write(arg[0]);
  Wire.write(arg[1]);
  Wire.write(crc);
  if (Wire.endTransmission() != 0) return false;

  kind_ = PM_SPS30;
  return true;
}

bool PmAir::readSps30(PmData& d) {
  if (!i2cuWrite16(PM_ADDR, PM_SPS_CMD_READ)) return false;
  delay(20);

  uint8_t buf[24];   // 4 liczby float po 2 słowa, każde słowo z CRC
  if (!i2cuRead(PM_ADDR, buf, sizeof(buf))) return false;
  if (!i2cuCheckCrc3(buf, 8)) return false;

  float f[4];
  for (uint8_t i = 0; i < 4; i++) {
    uint32_t hi = (uint32_t)((buf[i * 6 + 0] << 8) | buf[i * 6 + 1]);
    uint32_t lo = (uint32_t)((buf[i * 6 + 3] << 8) | buf[i * 6 + 4]);
    uint32_t bits = (hi << 16) | lo;
    memcpy(&f[i], &bits, sizeof(float));
    if (!isfinite(f[i]) || f[i] < 0.0f || f[i] > 100000.0f) f[i] = NAN;
  }
  d.pm1 = f[0];
  d.pm25 = f[1];
  d.pm4 = f[2];
  d.pm10 = f[3];
  return true;
}

bool PmAir::read(PmData& d) {
  switch (kind_) {
    case PM_SEN5X: return readSen5x(d);
    case PM_SPS30: return readSps30(d);
    default: return false;
  }
}
