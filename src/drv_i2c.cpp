/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_i2c.h"

// =============================================================
//  Sterowniki dodatkowych czujników I2C.
//
//  Zasada: begin() = sprawdzenie sprzętu (WHO AM I / odpowiedź na
//  komendę), read() = odczyt i przeliczenie na jednostki.
//  Jeśli układ nie odpowie, begin() zwraca false i kanał zostaje
//  "niewykryty" - żadne dane nie są publikowane.
// =============================================================

// ---------- pomocniki lokalne ----------

// Rejestry 16-bit little-endian (VEML7700, AS5600)
static bool readReg16le(uint8_t addr, uint8_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!i2cuReadReg(addr, reg, b, 2)) return false;
  v = (uint16_t)(b[0] | (b[1] << 8));
  return true;
}

static bool writeReg16le(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(v & 0xFF));
  Wire.write((uint8_t)(v >> 8));
  return Wire.endTransmission() == 0;
}

// Rejestry 16-bit big-endian (ADS1115)
static bool readReg16be(uint8_t addr, uint8_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!i2cuReadReg(addr, reg, b, 2)) return false;
  v = (uint16_t)((b[0] << 8) | b[1]);
  return true;
}

static bool writeReg16be(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(v >> 8));
  Wire.write((uint8_t)(v & 0xFF));
  return Wire.endTransmission() == 0;
}

// =============================================================
//  SHT4x (SHT40 / SHT41 / SHT45)
// =============================================================
#define SHT4X_CMD_MEASURE_HIGH 0xFD   // pomiar wysokiej precyzji, ~10 ms

bool Sht4x::begin() {
  static const uint8_t kAddr[3] = {0x44, 0x45, 0x46};
  for (uint8_t i = 0; i < 3; i++) {
    if (!i2cuPresent(kAddr[i])) continue;

    // SHT3x siedzi pod tym samym adresem, ale nie zna komendy 0xFD,
    // więc dodatkowo sprawdzamy CRC odpowiedzi - dzięki temu nie
    // pomylimy SHT3x z SHT4x.
    if (!i2cuWrite8(kAddr[i], SHT4X_CMD_MEASURE_HIGH)) continue;
    delay(12);
    uint8_t b[6];
    if (!i2cuRead(kAddr[i], b, 6)) continue;
    if (i2cuCrc8(b, 2) != b[2]) continue;
    if (i2cuCrc8(b + 3, 2) != b[5]) continue;

    addr_ = kAddr[i];
    ok_ = true;
    return true;
  }
  return false;
}

bool Sht4x::read(float& tempC, float& rh) {
  if (!ok_) return false;
  if (!i2cuWrite8(addr_, SHT4X_CMD_MEASURE_HIGH)) return false;
  delay(12);
  uint8_t b[6];
  if (!i2cuRead(addr_, b, 6)) return false;
  if (i2cuCrc8(b, 2) != b[2]) return false;
  if (i2cuCrc8(b + 3, 2) != b[5]) return false;

  uint32_t rawT = ((uint32_t)b[0] << 8) | b[1];
  uint32_t rawH = ((uint32_t)b[3] << 8) | b[4];
  tempC = -45.0f + 175.0f * (float)rawT / 65535.0f;
  rh = -6.0f + 125.0f * (float)rawH / 65535.0f;
  rh = constrain(rh, 0.0f, 100.0f);
  return true;
}

// =============================================================
//  BMP581 / BMP580 (ciśnienie + temperatura)
// =============================================================
#define BMP5_REG_CHIP_ID   0x01
#define BMP5_REG_DATA      0x1D   // TEMP_XLSB .. PRESS_MSB (6 bajtów)
#define BMP5_REG_DSP_CONFIG 0x30
#define BMP5_REG_DSP_IIR   0x31
#define BMP5_REG_OSR_CONFIG 0x36
#define BMP5_REG_ODR_CONFIG 0x37
#define BMP5_REG_CMD       0x7E
#define BMP5_CMD_SOFT_RESET 0xB6

bool Bmp581::begin() {
  static const uint8_t kAddr[2] = {0x46, 0x47};
  for (uint8_t i = 0; i < 2; i++) {
    if (!i2cuPresent(kAddr[i])) continue;
    uint8_t id = 0;
    if (!i2cuReadReg(kAddr[i], BMP5_REG_CHIP_ID, &id, 1)) continue;
    if (id != 0x50 && id != 0x51) continue;

    addr_ = kAddr[i];
    // Miękki reset - po resecie ESP32 układ może zostać w trybie FIFO
    // po poprzednim uruchomieniu, reset daje powtarzalny start.
    i2cuWriteReg(addr_, BMP5_REG_CMD, BMP5_CMD_SOFT_RESET);
    delay(10);

    // Filtr IIR (współczynnik 1) włączony dla danych zapisywanych do rejestrów
    i2cuWriteReg(addr_, BMP5_REG_DSP_CONFIG, 0x28);   // shdw_set_iir_t + iir_p
    i2cuWriteReg(addr_, BMP5_REG_DSP_IIR, 0x09);      // iir_t = 1, iir_p = 1

    // 2x oversampling temperatury, 8x ciśnienia, ciśnienie włączone
    i2cuWriteReg(addr_, BMP5_REG_OSR_CONFIG, 0x59);   // osr_t=1, osr_p=3, press_en

    // Tryb normalny (pomiar ciągły), ODR na najwyższym zakresie.
    // Odczyt i tak robimy raz na 10 s, więc ODR nie ma znaczenia dla
    // zużycia, a wysoki ODR gwarantuje świeże dane w rejestrach.
    i2cuWriteReg(addr_, BMP5_REG_ODR_CONFIG, 0x09);   // pwr_mode=1 (normal), odr=2
    ok_ = true;
    return true;
  }
  return false;
}

bool Bmp581::read(float& tempC, float& pressHpa) {
  if (!ok_) return false;
  uint8_t b[6];
  if (!i2cuReadReg(addr_, BMP5_REG_DATA, b, 6)) return false;

  int32_t rawT = ((int32_t)b[2] << 16) | ((int32_t)b[1] << 8) | b[0];
  int32_t rawP = ((int32_t)b[5] << 16) | ((int32_t)b[4] << 8) | b[3];
  if (rawT & 0x800000) rawT -= 0x1000000;   // dane 24-bit ze znakiem
  if (rawP & 0x800000) rawP -= 0x1000000;

  tempC = (float)rawT / 65536.0f;           // °C, LSB = 1/2^16
  pressHpa = (float)rawP / 6400.0f;         // LSB = 1/64 Pa -> hPa
  return true;
}

// =============================================================
//  VEML7700 (światło, zakres do ~15 klx)
// =============================================================
#define VEML7700_ADDR        0x10
#define VEML7700_REG_CONF    0x00
#define VEML7700_REG_ALS     0x04
#define VEML7700_REG_ID      0x07

bool Veml7700::begin() {
  if (!i2cuPresent(VEML7700_ADDR)) return false;
  uint16_t id = 0;
  if (!readReg16le(VEML7700_ADDR, VEML7700_REG_ID, id)) return false;
  if (id != 0x0001) return false;

  // Wzmocnienie 1/8 (bit12:11 = 0b10), czas integracji 100 ms (bit9:6 = 0),
  // oszczędzanie energii wyłączone, przerwania wyłączone, pomiar włączony.
  if (!writeReg16le(VEML7700_ADDR, VEML7700_REG_CONF, 0x1000)) return false;
  ok_ = true;
  return true;
}

bool Veml7700::read(float& lux) {
  if (!ok_) return false;
  uint16_t raw = 0;
  if (!readReg16le(VEML7700_ADDR, VEML7700_REG_ALS, raw)) return false;
  // 0,0036 lx/zb * (800/100 ms) / (1/8) = 0,2304 lx/zb
  lux = (float)raw * 0.2304f;
  return true;
}

// =============================================================
//  LTR-390UV (indeks UV)
// =============================================================
#define LTR390_ADDR        0x53
#define LTR390_REG_MAIN    0x00
#define LTR390_REG_RATE    0x04
#define LTR390_REG_GAIN    0x05
#define LTR390_REG_PARTID  0x06
#define LTR390_REG_STATUS  0x07
#define LTR390_REG_UVS     0x10

bool Ltr390uv::begin() {
  if (!i2cuPresent(LTR390_ADDR)) return false;
  uint8_t id = 0;
  if (!i2cuReadReg(LTR390_ADDR, LTR390_REG_PARTID, &id, 1)) return false;
  if ((id >> 4) != 0x0B) return false;   // LTR-390UV: 0xBx

  i2cuWriteReg(LTR390_ADDR, LTR390_REG_MAIN, 0x10);   // reset
  delay(12);
  // Rozdzielczość 20 bit / 400 ms (bit6:4 = 0), pomiar co 1000 ms (bit2:0 = 5)
  if (!i2cuWriteReg(LTR390_ADDR, LTR390_REG_RATE, 0x05)) return false;
  // Wzmocnienie 18x (indeks UV)
  if (!i2cuWriteReg(LTR390_ADDR, LTR390_REG_GAIN, 0x04)) return false;
  // Tryb UVS (bit3) + pomiar włączony (bit1)
  if (!i2cuWriteReg(LTR390_ADDR, LTR390_REG_MAIN, 0x0A)) return false;
  ok_ = true;
  return true;
}

bool Ltr390uv::read(float& uvIndex) {
  if (!ok_) return false;
  // Czekamy na gotowe dane (bit3), maksymalnie ~450 ms
  for (uint8_t i = 0; i < 30; i++) {
    uint8_t st = 0;
    if (!i2cuReadReg(LTR390_ADDR, LTR390_REG_STATUS, &st, 1)) return false;
    if (st & 0x08) break;
    delay(15);
  }
  uint8_t b[3];
  if (!i2cuReadReg(LTR390_ADDR, LTR390_REG_UVS, b, 3)) return false;
  uint32_t raw = ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
  // Czułość 2300 dla wzmocnienia 18x i czasu integracji 400 ms
  uvIndex = (float)raw / 2300.0f;
  if (uvIndex > 20.0f) uvIndex = 20.0f;
  return true;
}

// =============================================================
//  MMC5983MA (kompas - azymut)
// =============================================================
#define MMC_ADDR_DATA   0x00
#define MMC_REG_IC0     0x09
#define MMC_REG_IC1     0x0A
#define MMC_REG_IC2     0x0B
#define MMC_REG_PRODID  0x2F

bool Mmc5983::begin() {
  static const uint8_t kAddr[2] = {0x30, 0x31};
  for (uint8_t i = 0; i < 2; i++) {
    if (!i2cuPresent(kAddr[i])) continue;
    uint8_t id = 0;
    if (!i2cuReadReg(kAddr[i], MMC_REG_PRODID, &id, 1)) continue;
    if (id != 0x30) continue;

    // Automatyczny SET/RESET (bit5) + pomiar ciągły 100 Hz (bit3, freq=5)
    i2cuWriteReg(kAddr[i], MMC_REG_IC0, 0x20);
    i2cuWriteReg(kAddr[i], MMC_REG_IC2, 0x0D);
    delay(10);
    addr_ = kAddr[i];
    ok_ = true;
    return true;
  }
  return false;
}

bool Mmc5983::readHeading(float& headingDeg) {
  if (!ok_) return false;

  // SET przed odczytem - usuwa dryf zera czujnika (mostek magnetyczny)
  if (!i2cuWriteReg(addr_, MMC_REG_IC0, 0x28)) return false;   // SET + auto_sr
  delay(2);

  uint8_t b[7];
  if (!i2cuReadReg(addr_, MMC_ADDR_DATA, b, 7)) return false;

  int32_t x = ((int32_t)b[0] << 10) | ((int32_t)b[1] << 2) | ((b[6] >> 6) & 0x03);
  int32_t y = ((int32_t)b[2] << 10) | ((int32_t)b[3] << 2) | ((b[6] >> 4) & 0x03);
  // int32_t z = ((int32_t)b[4] << 10) | ((int32_t)b[5] << 2) | ((b[6] >> 2) & 0x03);
  const float kOffset = 131072.0f;   // zero pola magnetycznego
  const float kCountsPerGauss = 16384.0f;
  float xg = (x - kOffset) / kCountsPerGauss;
  float yg = (y - kOffset) / kCountsPerGauss;

  float deg = atan2f(yg, xg) * 180.0f / (float)PI;
  if (deg < 0.0f) deg += 360.0f;
  headingDeg = deg;
  return true;
}

// =============================================================
//  AS5600 (magnetyczny enkoder kąta - może zastąpić potencjometr
//  wiatrowskazu, gdy pin analogowy nie jest używany)
// =============================================================
#define AS5600_ADDR      0x36
#define AS5600_REG_STATUS 0x0B
#define AS5600_REG_ANGLE  0x0E

bool As5600::begin() {
  if (!i2cuPresent(AS5600_ADDR)) return false;
  uint8_t st = 0;
  if (!i2cuReadReg(AS5600_ADDR, AS5600_REG_STATUS, &st, 1)) return false;
  uint16_t ang = 0;
  if (!readReg16le(AS5600_ADDR, AS5600_REG_ANGLE, ang)) return false;
  if (ang > 0x0FFF) return false;          // kąt jest 12-bitowy
  magnetOk_ = (((st >> 3) & 0x07) == 0x01);
  ok_ = true;
  return true;
}

bool As5600::readAngle(float& deg) {
  if (!ok_) return false;
  uint8_t st = 0;
  if (i2cuReadReg(AS5600_ADDR, AS5600_REG_STATUS, &st, 1)) {
    magnetOk_ = (((st >> 3) & 0x07) == 0x01);
  }
  uint16_t raw = 0;
  if (!readReg16le(AS5600_ADDR, AS5600_REG_ANGLE, raw)) return false;
  deg = (float)(raw & 0x0FFF) * 360.0f / 4096.0f;
  return true;
}

// =============================================================
//  ADS1115 (4-kanalowy ADC 16 bit)
// =============================================================
#define ADS_REG_CONVERSION 0x00
#define ADS_REG_CONFIG     0x01

// PGA 001 = zakres +/-4,096 V -> 1 LSB = 125 uV
static const float ADS_LSB_VOLTS = 4.096f / 32768.0f;

bool Ads1115::begin() {
  for (uint8_t a = 0x48; a <= 0x4B; a++) {
    if (!i2cuPresent(a)) continue;
    // Rejestr konfiguracji po zapisie oddaje dokładnie to, co zapisano -
    // to najprostszy test, że pod adresem naprawdę siedzi ADS1115.
    const uint16_t probe = 0xC383;   // start, AIN0/GND, +/-4,096 V, 128 SPS
    if (!writeReg16be(a, ADS_REG_CONFIG, probe)) continue;
    uint16_t rb = 0;
    if (!readReg16be(a, ADS_REG_CONFIG, rb)) continue;
    if (rb != probe) continue;
    addr_ = a;
    ok_ = true;
    return true;
  }
  return false;
}

bool Ads1115::readChannel(uint8_t ch, float& volts) {
  if (!ok_ || ch > 3) return false;
  // MUX 100..111 = AINx względem GND, MODE = pojedynczy pomiar
  uint16_t cfg = (uint16_t)(0x8000 | ((0x04 + ch) << 12) | 0x0200 | 0x0100 | 0x0080 | 0x0003);
  if (!writeReg16be(addr_, ADS_REG_CONFIG, cfg)) return false;

  delay(10);   // 128 SPS = 8 ms na konwersję

  uint16_t raw = 0;
  if (!readReg16be(addr_, ADS_REG_CONVERSION, raw)) return false;
  int16_t s = (int16_t)raw;
  volts = (float)s * ADS_LSB_VOLTS;
  return true;
}
