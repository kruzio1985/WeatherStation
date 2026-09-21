/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */

// =============================================================
//  Moduł sterowników ŚWIATŁO / UV / ZASILANIE.
//
//  Wszystko ręcznie - tylko Wire i pomocniki z i2c_util.h,
//  bez żadnych bibliotek zewnętrznych.
//
//  Światło:
//    * OPT3001              - 0x44 / 0x45   (MANUFACTURER_ID 0x7E = 0x5449)
//    * TSL2591              - 0x29          (ID 0x12 = 0x50)
//    * TSL2561              - 0x29/0x39/0x49 (ID 0x0A, górna połowa = 0x50)
//    * LTR-329 / LTR-303    - 0x29          (MANUFACTURER_ID 0x07 = 0x05)
//  UV:
//    * VEML6070             - 0x38 (komenda) + 0x39 (MSB) - układ bez ID
//    * VEML6075             - 0x10          (ID 0x0C = 0x26)
//    * SI1145               - 0x60          (PART_ID 0x00 = 0x45)
//  Zasilanie:
//    * INA219 / INA226 / INA228 - 0x40..0x4F (0xFE = 0x5449, DIE_ID 0xFF)
//    * INA3221                  - 0x40..0x43 (3 kanały)
//    * MAX17048 / MAX17049      - 0x36 (VERSION 0x08 = 0x0011/0x0012)
//    * LC709203F                - 0x0B (IC_VERSION 0x11 = 0x0010)
//
//  Rozpoznanie wyłącznie po ID układu - jeżeli ID się nie zgadza, kanał
//  zostaje pusty i nie ma żadnego logu ostrzegawczego. Konflikty adresów
//  rozstrzygamy po ID: pod 0x29 siedzi TSL2591 albo TSL2561, albo LTR-329
//  i wygrywa ten, który potwierdzi swoje ID. Obsługujemy tylko pierwszy
//  potwierdzony układ danego typu - kolejny zgłaszamy jako pominięty.
//
//  VEML6070 nie ma rejestru ID, więc rozpoznajemy go po adresie i sensownej
//  odpowiedzi; sprawdzamy go na końcu i tylko wtedy, gdy adres 0x39 (jego
//  bajt MSB) nie należy do TSL2561.
//
//  Czujniki analogowe (ACS712, GUVA-S12SD) tu nie należą - siedzą na
//  ADC / ADS1115 i obsługuje je osobny moduł.
// =============================================================
#include "drv_light.h"
#include "config.h"
#include "syslog.h"
#include "i2c_util.h"
#include <Wire.h>
#include <math.h>

// =============================================================
//  Rejestry i stałe układów
// =============================================================

// --- OPT3001 (0x44 / 0x45) ---
#define OPT3001_ADDR_A          0x44
#define OPT3001_ADDR_B          0x45
#define OPT3001_REG_RESULT      0x00
#define OPT3001_REG_CONFIG      0x01
#define OPT3001_REG_MANUF_ID    0x7E
#define OPT3001_REG_DEVICE_ID   0x7F
#define OPT3001_MANUF_ID        0x5449
#define OPT3001_DEVICE_ID       0x3001
// RN = 0xC (zakres automatyczny), CT = 1 (800 ms), MODE = 10 (praca ciągła)
#define OPT3001_CONFIG_CONT     0xCC00

// --- TSL2591 (0x29) ---
#define TSL2591_ADDR            0x29
#define TSL2591_CMD             0x80     // bit komendy
#define TSL2591_CMD_BLOCK       0xA0     // komenda + blok (auto-inkrementacja)
#define TSL2591_REG_ENABLE      0x00
#define TSL2591_REG_CONTROL     0x01
#define TSL2591_REG_ID          0x12
#define TSL2591_REG_C0          0x14
#define TSL2591_REG_C1          0x16
#define TSL2591_ID              0x50

// --- TSL2561 (0x29 / 0x39 / 0x49) ---
#define TSL2561_CMD             0x80
#define TSL2561_REG_CONTROL     0x00
#define TSL2561_REG_TIMING      0x01
#define TSL2561_REG_ID          0x0A
#define TSL2561_REG_DATA0       0x0C
#define TSL2561_REG_DATA1       0x0E
#define TSL2561_ID_MASK         0xF0
#define TSL2561_ID_BASE         0x50

// --- LTR-329 / LTR-303 (0x29) ---
#define LTR_ADDR                0x29
#define LTR_REG_ALS_CONTR       0x00
#define LTR_REG_ALS_MEAS_RATE   0x05
#define LTR_REG_PART_ID         0x06
#define LTR_REG_MANUF_ID        0x07
#define LTR_REG_CH1             0x08
#define LTR_REG_CH0             0x0A
#define LTR_MANUF_ID            0x05
#define LTR_CONTR_ACTIVE_1X     0x01     // aktywny, wzmocnienie 1x
#define LTR_RATE_100MS          0x02     // całkowanie 100 ms, pomiar co 200 ms

// --- VEML6070 (0x38 komenda + 0x39 dane MSB) ---
#define VEML6070_ADDR_CMD       0x38
#define VEML6070_ADDR_MSB       0x39
#define VEML6070_CMD_IT1        0x04     // IT = 1T (62,5 ms)
// Współczynnik dla RSET = 270k i IT = 1T (~1/230), korygowany z configu
#define VEML6070_UV_SCALE       0.0043478f

// --- VEML6075 (0x10) ---
#define VEML6075_ADDR           0x10
#define VEML6075_REG_CONF       0x00
#define VEML6075_REG_UVA        0x07
#define VEML6075_REG_UVB        0x09
#define VEML6075_REG_COMP1      0x0A
#define VEML6075_REG_COMP2      0x0B
#define VEML6075_REG_ID         0x0C
#define VEML6075_ID             0x26
// UV_IT = 1 (100 ms), SD = 0 (układ aktywny)
#define VEML6075_CONF_100MS     0x0010
#define VEML6075_UVA_A          2.22f    // współczynniki kompensacji
#define VEML6075_UVA_B          1.33f
#define VEML6075_UVB_C          2.95f
#define VEML6075_UVB_D          1.74f

// --- SI1145 (0x60) ---
#define SI1145_ADDR             0x60
#define SI1145_REG_PART_ID      0x00
#define SI1145_REG_COMMAND      0x18
#define SI1145_REG_HWKEY        0x07
#define SI1145_REG_MEAS_RATE0   0x08
#define SI1145_REG_UCOEFF0      0x13
#define SI1145_REG_PARAMWR      0x17
#define SI1145_REG_VIS          0x22
#define SI1145_REG_IR           0x24
#define SI1145_REG_UV           0x2C
#define SI1145_PART_ID          0x45
#define SI1145_CMD_PARAM_SET    0xA0
#define SI1145_CMD_ALS_AUTO     0x0E
#define SI1145_CMD_RESET        0x01
#define SI1145_PARAM_CHLIST     0x01
#define SI1145_CHLIST_ENUV      0x80
#define SI1145_CHLIST_ENALSIR   0x20
#define SI1145_CHLIST_ENALSVIS  0x10

// --- INA219 / INA226 / INA228 / INA3221 (0x40..0x4F) ---
#define INA_REG_CONFIG          0x00
#define INA_REG_BUS_VOLTAGE     0x02
#define INA_REG_POWER           0x03
#define INA_REG_CURRENT         0x04
#define INA_REG_CALIBRATION     0x05
#define INA_REG_MANUF_ID        0xFE
#define INA_REG_DIE_ID          0xFF
#define INA_MANUF_ID            0x5449
#define INA219_DIE_ID           0x399F
#define INA226_DIE_ID           0x2260
#define INA228_DIE_ID           0x2280
#define INA3221_DIE_ID          0x3220
#define INA219_CONFIG           0x399F   // 32 V, zakres +/-320 mV, 12 bit
#define INA226_CONFIG           0x4127   // średnia 16x, 1,1 ms, ciągły
#define INA3221_CONFIG          0x7127   // 3 kanały, 1,1 ms, ciągły
// INA228 ma własna mapę rejestrów (nie odpowiada na 0xFE/0xFF)
#define INA228_REG_ADC_CONFIG   0x01
#define INA228_REG_VSHUNT       0x04
#define INA228_REG_VBUS         0x05
#define INA228_REG_MANUF_ID     0x3E
#define INA228_REG_DEVICE_ID    0x3F
#define INA228_ADC_RANGE_BIT    0x0010   // bit ADCRANGE
#define INA228_VSHUNT_LSB_ADCR1 78.125e-9f   // 78,125 nV/LSB
#define INA228_VSHUNT_LSB_ADCR0 312.5e-9f    // 312,5 nV/LSB
#define INA228_VBUS_LSB         195.3125e-6f // 195,3125 uV/LSB

#define INA_KIND_NONE           0
#define INA_KIND_219            1
#define INA_KIND_226            2
#define INA_KIND_228            3
#define INA_KIND_3221           4

// --- MAX17048 / MAX17049 (0x36) ---
#define MAX17048_ADDR           0x36
#define MAX17048_REG_VCELL      0x02
#define MAX17048_REG_SOC        0x04
#define MAX17048_REG_VERSION    0x08
#define MAX17048_VERSION_48     0x0011
#define MAX17048_VERSION_49     0x0012
#define MAX17048_VCELL_LSB      78.125e-6f

// --- LC709203F (0x0B) ---
#define LC709203F_ADDR          0x0B
#define LC709203F_REG_VOLTAGE   0x09
#define LC709203F_REG_RSOC      0x0D
#define LC709203F_REG_IC_VERSION 0x11
#define LC709203F_REG_POWER_MODE 0x15
#define LC709203F_IC_VERSION    0x0010
#define LC709203F_MODE_OPERATE  0x0001

// =============================================================
//  Tabele pomocnicze
// =============================================================

// Wzmocnienia TSL2591: indeks rejestru CONTROL (bity 5..4)
static const float TSL2591_AGAIN[4] = { 1.0f, 25.0f, 428.0f, 9876.0f };

// Wzmocnienie TSL2561: 0 = 1x, 1 = 16x
static const float TSL2561_GAIN[2] = { 1.0f, 16.0f };

// Czas integracji TSL2561 w ms (kolejność = kody rejestru TIMING)
static const uint16_t TSL2561_TIME[3] = { 13, 101, 402 };

// =============================================================
//  Kanały modułu (id / nazwa / jednostka / miejsca / strefa / HA)
// =============================================================
static const ChanDef CHANS[] = {
  { "lt_opt3001",      "Natężenie światła (OPT3001)",     "lx", 0, "out", "illuminance", "lx", "mdi:white-balance-sunny" },
  { "lt_tsl2591",      "Natężenie światła (TSL2591)",     "lx", 0, "out", "illuminance", "lx", "mdi:white-balance-sunny" },
  { "lt_tsl2561",      "Natężenie światła (TSL2561)",     "lx", 0, "out", "illuminance", "lx", "mdi:white-balance-sunny" },
  { "lt_ltr329",       "Natężenie światła (LTR-329)",     "lx", 0, "out", "illuminance", "lx", "mdi:white-balance-sunny" },
  { "lt_veml6070_uv",  "Indeks UV (VEML6070)",            "",   1, "out", "",            "",   "mdi:weather-sunny-alert" },
  { "lt_veml6075_uvi", "Indeks UV (VEML6075)",            "",   1, "out", "",            "",   "mdi:weather-sunny-alert" },
  { "lt_veml6075_uva", "UVA (VEML6075)",                  "",   1, "out", "",            "",   "mdi:weather-sunny-alert" },
  { "lt_veml6075_uvb", "UVB (VEML6075)",                  "",   1, "out", "",            "",   "mdi:weather-sunny-alert" },
  { "lt_si1145_uv",    "Indeks UV (SI1145)",              "",   1, "out", "",            "",   "mdi:weather-sunny-alert" },
  { "lt_si1145_vis",   "Światło widzialne (SI1145)",      "",   0, "out", "",            "",   "mdi:brightness-6" },
  { "lt_si1145_ir",    "Podczerwień (SI1145)",            "",   0, "out", "",            "",   "mdi:brightness-5" },
  { "pw_ina219_v",     "Napięcie (INA219)",               "V",  2, "in",  "voltage",     "V",  "mdi:flash" },
  { "pw_ina219_i",     "Prąd (INA219)",                   "A",  3, "in",  "current",     "A",  "mdi:current-dc" },
  { "pw_ina219_p",     "Moc (INA219)",                    "W",  2, "in",  "power",       "W",  "mdi:flash" },
  { "pw_ina226_v",     "Napięcie (INA226)",               "V",  2, "in",  "voltage",     "V",  "mdi:flash" },
  { "pw_ina226_i",     "Prąd (INA226)",                   "A",  3, "in",  "current",     "A",  "mdi:current-dc" },
  { "pw_ina226_p",     "Moc (INA226)",                    "W",  2, "in",  "power",       "W",  "mdi:flash" },
  { "pw_ina3221_v1",   "Napięcie kanał 1 (INA3221)",      "V",  2, "in",  "voltage",     "V",  "mdi:flash" },
  { "pw_ina3221_i1",   "Prąd kanał 1 (INA3221)",          "A",  3, "in",  "current",     "A",  "mdi:current-dc" },
  { "pw_ina3221_v2",   "Napięcie kanał 2 (INA3221)",      "V",  2, "in",  "voltage",     "V",  "mdi:flash" },
  { "pw_ina3221_i2",   "Prąd kanał 2 (INA3221)",          "A",  3, "in",  "current",     "A",  "mdi:current-dc" },
  { "pw_ina3221_v3",   "Napięcie kanał 3 (INA3221)",      "V",  2, "in",  "voltage",     "V",  "mdi:flash" },
  { "pw_ina3221_i3",   "Prąd kanał 3 (INA3221)",          "A",  3, "in",  "current",     "A",  "mdi:current-dc" },
  { "pw_bat_soc",      "Naładowanie baterii (MAX17048)",  "%",  0, "in",  "battery",     "%",  "mdi:battery" },
  { "pw_bat_v",        "Napięcie baterii (MAX17048)",     "V",  2, "in",  "voltage",     "V",  "mdi:battery" },
  { "pw_lc709203_soc", "Naładowanie baterii (LC709203F)", "%",  0, "in",  "battery",     "%",  "mdi:battery" },
  { "pw_lc709203_v",   "Napięcie baterii (LC709203F)",    "V",  3, "in",  "voltage",     "V",  "mdi:battery" }
};

#define NCHANS (sizeof(CHANS) / sizeof(CHANS[0]))

// Identyfikatory kanałów INA3221 - bez alokacji w readImpl()
static const char* const INA3221_VID[3] = { "pw_ina3221_v1", "pw_ina3221_v2", "pw_ina3221_v3" };
static const char* const INA3221_IID[3] = { "pw_ina3221_i1", "pw_ina3221_i2", "pw_ina3221_i3" };

// =============================================================
//  Stan modułu (wypełniany w beginImpl)
// =============================================================
static DrvSink s_sink = { nullptr, nullptr, nullptr };

static bool    s_optOk    = false;
static uint8_t s_optAddr  = 0;

static bool    s_tsl2591Ok = false;
static uint8_t s_2591Gain = 0;
static uint16_t s_2591Time = 100;

static bool    s_tsl2561Ok = false;
static uint8_t s_2561Addr = 0;
static uint8_t s_2561Gain = 0;
static uint8_t s_2561TimeIdx = 2;

static bool    s_ltrOk = false;

static bool    s_veml6070Ok = false;
static bool    s_veml6075Ok = false;
static bool    s_si1145Ok = false;

static uint8_t s_inaKind = INA_KIND_NONE;
static uint8_t s_inaAddr = 0;
static float   s_ina228VshuntLsb = INA228_VSHUNT_LSB_ADCR0;

static bool    s_maxOk = false;
static bool    s_lcOk  = false;

// =============================================================
//  Pomocniki lokalne (i2c_util.h zostaje bez zmian)
// =============================================================

// Odczyt 16 bitów, bajt starszy pierwszy (OPT3001, INA219/226/3221)
static bool r16be(uint8_t addr, uint8_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!i2cuReadReg(addr, reg, b, 2)) return false;
  v = ((uint16_t)b[0] << 8) | b[1];
  return true;
}

// Odczyt 16 bitów, bajt młodszy pierwszy (TSL2591/TSL2561/LTR/VEML6075/LC709203F)
static bool r16le(uint8_t addr, uint8_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!i2cuReadReg(addr, reg, b, 2)) return false;
  v = ((uint16_t)b[1] << 8) | b[0];
  return true;
}

// Odczyt 24 bitów (INA228: VSHUNT/VBUS)
static bool r24be(uint8_t addr, uint8_t reg, uint32_t& v) {
  uint8_t b[3];
  if (!i2cuReadReg(addr, reg, b, 3)) return false;
  v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
  return true;
}

// Zapis 16 bitów, bajt starszy pierwszy
static bool w16be(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(v >> 8));
  Wire.write((uint8_t)(v & 0xFF));
  return Wire.endTransmission() == 0;
}

// Zapis 16 bitów, bajt młodszy pierwszy
static bool w16le(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(v & 0xFF));
  Wire.write((uint8_t)(v >> 8));
  return Wire.endTransmission() == 0;
}

// CRC-8 Maxim/Dallas (wielomian 0x07) - opcjonalny bajt kontrolny MAX17048/LC709203F
static uint8_t crc8Maxim(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Odczyt 3 bajtów z tolerancją na wyłączone CRC (3. bajt 0x00/0xFF albo zgodny CRC)
static bool readWithCrc(uint8_t addr, uint8_t reg, bool littleEndian, uint16_t& v) {
  uint8_t b[3];
  if (i2cuReadReg(addr, reg, b, 3)) {
    bool crcOk = (b[2] == 0x00) || (b[2] == 0xFF) || (b[2] == crc8Maxim(b, 2));
    if (crcOk) {
      v = littleEndian ? (uint16_t)(b[0] | ((uint16_t)b[1] << 8))
                       : (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
      return true;
    }
  }
  uint8_t c[2];
  if (!i2cuReadReg(addr, reg, c, 2)) return false;
  v = littleEndian ? (uint16_t)(c[0] | ((uint16_t)c[1] << 8))
                   : (uint16_t)(((uint16_t)c[0] << 8) | c[1]);
  return true;
}

// Kalibracja INA219/INA226: Cal = wspólczynnik / (I_LSB * R_shunt)
static uint16_t inaCalibration(float ilsb, float rshunt, float factor) {
  if (ilsb <= 0.0f || rshunt <= 0.0f) return 0;
  float cal = factor / (ilsb * rshunt);
  if (cal < 1.0f) cal = 1.0f;
  if (cal > 65535.0f) cal = 65535.0f;
  return (uint16_t)cal;
}

// =============================================================
//  Wykrywanie układów
// =============================================================

// OPT3001: MANUFACTURER_ID 0x7E = 0x5449 i DEVICE_ID 0x7F = 0x3001.
// Adres 0x44/0x45 dzielimy z innymi układami (np. SHT4x) - bez ID nic nie robimy.
static void detectOpt3001() {
  const uint8_t addrs[2] = { OPT3001_ADDR_A, OPT3001_ADDR_B };
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t a = addrs[i];
    uint16_t man = 0, dev = 0;
    if (!r16be(a, OPT3001_REG_MANUF_ID, man) || man != OPT3001_MANUF_ID) continue;
    if (!r16be(a, OPT3001_REG_DEVICE_ID, dev) || dev != OPT3001_DEVICE_ID) continue;
    if (s_optOk) {
      LOG_I("OPT3001: pomijam kolejny układ pod adresem 0x%02X - obsługiwany jest pierwszy", (unsigned)a);
      continue;
    }
    if (!w16be(a, OPT3001_REG_CONFIG, OPT3001_CONFIG_CONT)) continue;
    s_optAddr = a;
    s_optOk = true;
    s_sink.found("lt_opt3001");
    LOG_I("OPT3001: wykryty pod adresem 0x%02X (zakres auto, 800 ms)", (unsigned)a);
  }
}

// TSL2591: ID 0x12 = 0x50
static void detectTsl2591() {
  uint8_t id = 0;
  if (!i2cuReadReg(TSL2591_ADDR, (uint8_t)(TSL2591_CMD | TSL2591_REG_ID), &id, 1)) return;
  if (id != TSL2591_ID) return;

  int gi = (int)(config.extraF("tsl2591_gain", 0.0f) + 0.5f);
  if (gi < 0) gi = 0;
  if (gi > 3) gi = 3;
  s_2591Gain = (uint8_t)gi;

  int ms = (int)(config.extraF("tsl2591_atime", 100.0f) + 0.5f);
  int ai = (ms - 100) / 100;
  if (ai < 0) ai = 0;
  if (ai > 5) ai = 5;
  s_2591Time = (uint16_t)(100 + 100 * ai);

  // CONTROL: wzmocnienie (bity 5..4) + czas integracji (bity 2..0)
  if (!i2cuWriteReg(TSL2591_ADDR, (uint8_t)(TSL2591_CMD | TSL2591_REG_CONTROL),
                    (uint8_t)((s_2591Gain << 4) | ai))) return;
  // ENABLE: PON + AEN
  if (!i2cuWriteReg(TSL2591_ADDR, (uint8_t)(TSL2591_CMD | TSL2591_REG_ENABLE), 0x03)) return;

  s_tsl2591Ok = true;
  s_sink.found("lt_tsl2591");
  LOG_I("TSL2591: wykryty pod adresem 0x29 (wzmocnienie %u, %u ms)",
        (unsigned)s_2591Gain, (unsigned)s_2591Time);
}

// TSL2561: ID 0x0A, górna połowa = 0x50. Adres 0x29 tylko wtedy, gdy nie zajął go
// TSL2591 (inny układ pod tym adresem).
static void detectTsl2561() {
  const uint8_t addrs[3] = { 0x29, 0x39, 0x49 };
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t a = addrs[i];
    if (a == TSL2591_ADDR && s_tsl2591Ok) continue;   // 0x29 należy do TSL2591
    uint8_t id = 0;
    if (!i2cuReadReg(a, (uint8_t)(TSL2561_CMD | TSL2561_REG_ID), &id, 1)) continue;
    if ((id & TSL2561_ID_MASK) != TSL2561_ID_BASE) continue;
    if (s_tsl2561Ok) {
      LOG_I("TSL2561: pomijam kolejny układ pod adresem 0x%02X - obsługiwany jest pierwszy", (unsigned)a);
      continue;
    }

    int gi = (config.extraF("tsl2561_gain", 0.0f) >= 0.5f) ? 1 : 0;
    s_2561Gain = (uint8_t)gi;

    int ms = (int)(config.extraF("tsl2561_atime", 402.0f) + 0.5f);
    uint8_t ti = (ms < 50) ? 0 : ((ms < 250) ? 1 : 2);
    s_2561TimeIdx = ti;

    // CONTROL: PON; TIMING: wzmocnienie (bit 4) + czas integracji (bity 1..0)
    if (!i2cuWriteReg(a, (uint8_t)(TSL2561_CMD | TSL2561_REG_CONTROL), 0x03)) continue;
    if (!i2cuWriteReg(a, (uint8_t)(TSL2561_CMD | TSL2561_REG_TIMING),
                      (uint8_t)((s_2561Gain ? 0x10 : 0x00) | ti))) continue;

    s_2561Addr = a;
    s_tsl2561Ok = true;
    s_sink.found("lt_tsl2561");
    LOG_I("TSL2561: wykryty pod adresem 0x%02X (gain %ux, %u ms)",
          (unsigned)a, (unsigned)(s_2561Gain ? 16 : 1), (unsigned)TSL2561_TIME[s_2561TimeIdx]);
  }
}

// LTR-329 / LTR-303: MANUFACTURER_ID 0x07 = 0x05 (adres 0x29, jeśli wolny)
static void detectLtr329() {
  if (s_tsl2591Ok || s_tsl2561Ok) return;   // 0x29 zajęty przez inny obsługiwany układ
  uint8_t man = 0, part = 0;
  if (!i2cuReadReg(LTR_ADDR, LTR_REG_MANUF_ID, &man, 1)) return;
  if (man != LTR_MANUF_ID) return;
  i2cuReadReg(LTR_ADDR, LTR_REG_PART_ID, &part, 1);   // tylko do logu

  if (!i2cuWriteReg(LTR_ADDR, LTR_REG_ALS_CONTR, LTR_CONTR_ACTIVE_1X)) return;
  if (!i2cuWriteReg(LTR_ADDR, LTR_REG_ALS_MEAS_RATE, LTR_RATE_100MS)) return;

  s_ltrOk = true;
  s_sink.found("lt_ltr329");
  LOG_I("LTR-329/303: wykryty pod adresem 0x29 (part id 0x%02X, 100 ms)", (unsigned)part);
}

// VEML6070 nie ma rejestru ID - rozpoznajemy po adresie i sensownej odpowiedzi.
// Adres 0x39 (bajt MSB) może należeć do TSL2561 - wtedy układ pomijamy.
static void detectVeml6070() {
  if (!i2cuPresent(VEML6070_ADDR_CMD)) return;
  if (s_tsl2561Ok && s_2561Addr == VEML6070_ADDR_MSB) {
    LOG_I("VEML6070: pomijam - adres 0x39 należy do TSL2561");
    return;
  }
  if (!i2cuWrite8(VEML6070_ADDR_CMD, VEML6070_CMD_IT1)) return;
  uint8_t msb = 0, lsb = 0;
  if (!i2cuRead(VEML6070_ADDR_MSB, &msb, 1)) return;
  if (!i2cuRead(VEML6070_ADDR_CMD, &lsb, 1)) return;
  if (msb == 0xFF && lsb == 0xFF) return;   // brak sensownej odpowiedzi

  s_veml6070Ok = true;
  s_sink.found("lt_veml6070_uv");
  LOG_I("VEML6070: wykryty pod adresem 0x38 (kanał UV, IT = 1T)");
}

// VEML6075: ID 0x0C = 0x26
static void detectVeml6075() {
  uint16_t id = 0;
  if (!r16le(VEML6075_ADDR, VEML6075_REG_ID, id)) return;
  if ((uint8_t)(id & 0x00FF) != VEML6075_ID) return;
  if (!w16le(VEML6075_ADDR, VEML6075_REG_CONF, VEML6075_CONF_100MS)) return;

  s_veml6075Ok = true;
  s_sink.found("lt_veml6075_uvi");
  s_sink.found("lt_veml6075_uva");
  s_sink.found("lt_veml6075_uvb");
  LOG_I("VEML6075: wykryty pod adresem 0x10 (UVA/UVB, 100 ms)");
}

// Ustawienie parametru SI1145: PARAMWR = wartość, COMMAND = parametr | 0xA0
static bool si1145ParamSet(uint8_t param, uint8_t value) {
  if (!i2cuWriteReg(SI1145_ADDR, SI1145_REG_PARAMWR, value)) return false;
  if (!i2cuWriteReg(SI1145_ADDR, SI1145_REG_COMMAND, (uint8_t)(param | SI1145_CMD_PARAM_SET))) return false;
  return true;
}

// SI1145: PART_ID 0x00 = 0x45. Po resecie trzeba podać klucz sprzętowy
// i współczynniki UV, potem włączyć kanały i tryb automatyczny.
static void detectSi1145() {
  uint8_t id = 0;
  if (!i2cuReadReg(SI1145_ADDR, SI1145_REG_PART_ID, &id, 1) || id != SI1145_PART_ID) return;

  i2cuWriteReg(SI1145_ADDR, SI1145_REG_COMMAND, SI1145_CMD_RESET);
  delay(10);
  if (!i2cuWriteReg(SI1145_ADDR, SI1145_REG_HWKEY, 0x17)) return;
  delay(10);
  if (!i2cuWriteReg(SI1145_ADDR, SI1145_REG_UCOEFF0, 0x29)) return;
  if (!i2cuWriteReg(SI1145_ADDR, (uint8_t)(SI1145_REG_UCOEFF0 + 1), 0x89)) return;
  if (!i2cuWriteReg(SI1145_ADDR, (uint8_t)(SI1145_REG_UCOEFF0 + 2), 0x02)) return;
  if (!i2cuWriteReg(SI1145_ADDR, (uint8_t)(SI1145_REG_UCOEFF0 + 3), 0x00)) return;

  // CHLIST: UV + kanał IR + kanał widzialny
  if (!si1145ParamSet(SI1145_PARAM_CHLIST,
                      (uint8_t)(SI1145_CHLIST_ENUV | SI1145_CHLIST_ENALSIR | SI1145_CHLIST_ENALSVIS))) return;
  if (!i2cuWriteReg(SI1145_ADDR, SI1145_REG_MEAS_RATE0, 0xFF)) return;
  if (!i2cuWriteReg(SI1145_ADDR, SI1145_REG_COMMAND, SI1145_CMD_ALS_AUTO)) return;

  s_si1145Ok = true;
  s_sink.found("lt_si1145_uv");
  s_sink.found("lt_si1145_vis");
  s_sink.found("lt_si1145_ir");
  LOG_I("SI1145: wykryty pod adresem 0x60 (UV/IR/VIS, pomiar automatyczny)");
}

// INA3221 - konfiguracja 3 kanałów (bez kanału mocy)
static void startIna3221(uint8_t a) {
  if (!w16be(a, INA_REG_CONFIG, INA3221_CONFIG)) return;
  s_inaKind = INA_KIND_3221;
  s_inaAddr = a;
  s_sink.found("pw_ina3221_v1");
  s_sink.found("pw_ina3221_i1");
  s_sink.found("pw_ina3221_v2");
  s_sink.found("pw_ina3221_i2");
  s_sink.found("pw_ina3221_v3");
  s_sink.found("pw_ina3221_i3");
  LOG_I("INA3221: wykryty pod adresem 0x%02X (3 kanały)", (unsigned)a);
}

// INA219 - magistrala + prąd + moc
static void startIna219(uint8_t a) {
  if (!w16be(a, INA_REG_CONFIG, INA219_CONFIG)) return;
  float ilsb = config.extraF("ina219_ilsb", 0.0001f);
  float rsh = config.extraF("ina219_rshunt", 0.1f);
  if (!w16be(a, INA_REG_CALIBRATION, inaCalibration(ilsb, rsh, 0.04096f))) return;
  s_inaKind = INA_KIND_219;
  s_inaAddr = a;
  s_sink.found("pw_ina219_v");
  s_sink.found("pw_ina219_i");
  s_sink.found("pw_ina219_p");
  LOG_I("INA219: wykryty pod adresem 0x%02X (napięcie/prąd/moc)", (unsigned)a);
}

// INA226 - magistrala + prąd + moc
static void startIna226(uint8_t a) {
  if (!w16be(a, INA_REG_CONFIG, INA226_CONFIG)) return;
  float ilsb = config.extraF("ina226_ilsb", 0.001f);
  float rsh = config.extraF("ina226_rshunt", 0.1f);
  if (!w16be(a, INA_REG_CALIBRATION, inaCalibration(ilsb, rsh, 0.00512f))) return;
  s_inaKind = INA_KIND_226;
  s_inaAddr = a;
  s_sink.found("pw_ina226_v");
  s_sink.found("pw_ina226_i");
  s_sink.found("pw_ina226_p");
  LOG_I("INA226: wykryty pod adresem 0x%02X (napięcie/prąd/moc)", (unsigned)a);
}

// INA228 - 20-bitowy pomiar; moc liczymy sami z napięcia i prądu
static void startIna228(uint8_t a, bool adcRangeHigh) {
  s_ina228VshuntLsb = adcRangeHigh ? INA228_VSHUNT_LSB_ADCR1 : INA228_VSHUNT_LSB_ADCR0;
  s_inaKind = INA_KIND_228;
  s_inaAddr = a;
  s_sink.found("pw_ina226_v");
  s_sink.found("pw_ina226_i");
  s_sink.found("pw_ina226_p");
  LOG_I("INA228: wykryty pod adresem 0x%02X (napięcie/prąd/moc)", (unsigned)a);
}

// Skan 0x40..0x4F: najpierw MANUFACTURER_ID 0xFE = 0x5449 i DIE_ID 0xFF,
// a dla INA228 dodatkowo jego własna mapa rejestrów (0x3E/0x3F).
static void detectIna() {
  for (uint8_t a = 0x40; a <= 0x4F; a++) {
    uint16_t man = 0, die = 0;
    bool known = false;
    if (r16be(a, INA_REG_MANUF_ID, man) && man == INA_MANUF_ID &&
        r16be(a, INA_REG_DIE_ID, die)) {
      known = (die == INA219_DIE_ID) || (die == INA226_DIE_ID) ||
              (die == INA228_DIE_ID) || (die == INA3221_DIE_ID);
    }
    if (!known) {
      // INA228 nie odpowiada pod 0xFE/0xFF - sprawdzamy jego własną mapę
      uint16_t d = 0;
      if (r16be(a, INA228_REG_MANUF_ID, man) && man == INA_MANUF_ID &&
          r16be(a, INA228_REG_DEVICE_ID, d) && (d & 0x0FFF) == 0x228) {
        die = INA228_DIE_ID;
        known = true;
      }
    }
    if (!known) continue;

    if (s_inaKind != INA_KIND_NONE) {
      LOG_I("INA (die 0x%04X): pomijam układ pod adresem 0x%02X - obsługiwany jest pierwszy",
            (unsigned)die, (unsigned)a);
      continue;
    }

    if (die == INA219_DIE_ID) {
      startIna219(a);
    } else if (die == INA226_DIE_ID) {
      startIna226(a);
    } else if (die == INA3221_DIE_ID) {
      startIna3221(a);
    } else {
      // INA228: ADCRANGE (bit 4 rejestru ADC_CONFIG) decyduje o LSB napięcia bocznika
      uint16_t adcCfg = 0;
      bool rangeHigh = r16be(a, INA228_REG_ADC_CONFIG, adcCfg) && (adcCfg & INA228_ADC_RANGE_BIT);
      startIna228(a, rangeHigh);
    }
  }
}

// MAX17048 / MAX17049: VERSION 0x08 = 0x0011 (48) albo 0x0012 (49)
static void detectMax17048() {
  uint16_t ver = 0;
  if (!readWithCrc(MAX17048_ADDR, MAX17048_REG_VERSION, false, ver)) return;
  if (ver != MAX17048_VERSION_48 && ver != MAX17048_VERSION_49) return;

  s_maxOk = true;
  s_sink.found("pw_bat_soc");
  s_sink.found("pw_bat_v");
  LOG_I("MAX17048/49: wykryty pod adresem 0x36 (wersja 0x%04X)", (unsigned)ver);
}

// LC709203F: IC_VERSION 0x11 = 0x0010
static void detectLc709203() {
  uint16_t ver = 0;
  if (!readWithCrc(LC709203F_ADDR, LC709203F_REG_IC_VERSION, true, ver)) return;
  if (ver != LC709203F_IC_VERSION) return;

  w16le(LC709203F_ADDR, LC709203F_REG_POWER_MODE, LC709203F_MODE_OPERATE);

  s_lcOk = true;
  s_sink.found("pw_lc709203_soc");
  s_sink.found("pw_lc709203_v");
  LOG_I("LC709203F: wykryty pod adresem 0x0B (RSOC + napięcie ogniwa)");
}

// =============================================================
//  Odczyt i publikacja wyników
// =============================================================

// OPT3001: lux = 0,01 * 2^E * R (E - wykładnik, R - mantysa z rejestru wyniku)
static void readOpt3001() {
  uint16_t raw = 0;
  if (!r16be(s_optAddr, OPT3001_REG_RESULT, raw)) {
    s_sink.clear("lt_opt3001");
    return;
  }
  float lux = 0.01f * (float)(1UL << ((raw >> 12) & 0x0F)) * (float)(raw & 0x0FFF);
  s_sink.publish("lt_opt3001", lux);
}

// TSL2591: cpl = czas * wzmocnienie / 408, lux = max(lux1, lux2)
static void readTsl2591() {
  uint16_t c0 = 0, c1 = 0;
  if (!r16le(TSL2591_ADDR, (uint8_t)(TSL2591_CMD_BLOCK | TSL2591_REG_C0), c0) ||
      !r16le(TSL2591_ADDR, (uint8_t)(TSL2591_CMD_BLOCK | TSL2591_REG_C1), c1)) {
    s_sink.clear("lt_tsl2591");
    return;
  }
  if (c0 == 0xFFFF || c1 == 0xFFFF) {          // czujnik nasycony - górna granica zakresu
    s_sink.publish("lt_tsl2591", 88000.0f);
    return;
  }
  float cpl = ((float)s_2591Time * TSL2591_AGAIN[s_2591Gain]) / 408.0f;
  if (cpl <= 0.0f) {
    s_sink.clear("lt_tsl2591");
    return;
  }
  float l1 = ((float)c0 - 1.64f * (float)c1) / cpl;
  float l2 = (0.86f * (float)c0 - 1.87f * (float)c1) / cpl;
  float lux = fmaxf(l1, l2);
  if (lux < 0.0f) lux = 0.0f;
  s_sink.publish("lt_tsl2591", lux);
}

// TSL2561: algorytm z datasheetu dla 402 ms i wzmocnienia 1x,
// potem przeliczenie na ustawiony czas/wzmocnienie.
static void readTsl2561() {
  uint16_t ch0 = 0, ch1 = 0;
  if (!r16le(s_2561Addr, (uint8_t)(TSL2561_CMD | TSL2561_REG_DATA0), ch0) ||
      !r16le(s_2561Addr, (uint8_t)(TSL2561_CMD | TSL2561_REG_DATA1), ch1)) {
    s_sink.clear("lt_tsl2561");
    return;
  }
  float lux = 0.0f;
  if (ch0 > 0) {
    float ratio = (float)ch1 / (float)ch0;
    if (ratio <= 0.50f)      lux = 0.0304f   * (float)ch0 - 0.062f    * (float)ch0 * powf(ratio, 1.4f);
    else if (ratio <= 0.61f) lux = 0.0224f   * (float)ch0 - 0.031f    * (float)ch1;
    else if (ratio <= 0.80f) lux = 0.0128f   * (float)ch0 - 0.0153f   * (float)ch1;
    else if (ratio <= 1.30f) lux = 0.00146f  * (float)ch0 - 0.00112f  * (float)ch1;
    if (lux < 0.0f) lux = 0.0f;
    lux *= (402.0f / (float)TSL2561_TIME[s_2561TimeIdx]) / TSL2561_GAIN[s_2561Gain];
  }
  s_sink.publish("lt_tsl2561", lux);
}

// LTR-329 / LTR-303: współczynniki wg datasheetu, ratio = CH1 / (CH0 + CH1)
static void readLtr329() {
  uint16_t ch1 = 0, ch0 = 0;
  if (!r16le(LTR_ADDR, LTR_REG_CH1, ch1) || !r16le(LTR_ADDR, LTR_REG_CH0, ch0)) {
    s_sink.clear("lt_ltr329");
    return;
  }
  float lux = 0.0f;
  float sum = (float)ch0 + (float)ch1;
  if (sum > 0.0f) {
    float ratio = (float)ch1 / sum;
    if (ratio < 0.45f)      lux = 1.7743f * (float)ch0 + 1.1059f * (float)ch1;
    else if (ratio < 0.64f) lux = 4.2785f * (float)ch0 - 1.9548f * (float)ch1;
    else if (ratio < 0.85f) lux = 0.5926f * (float)ch0 + 0.1185f * (float)ch1;
    if (lux < 0.0f) lux = 0.0f;
  }
  s_sink.publish("lt_ltr329", lux);
}

// VEML6070: surowy odczyt 16 bitów przeliczony na indeks UV
static void readVeml6070() {
  uint8_t msb = 0, lsb = 0;
  if (!i2cuRead(VEML6070_ADDR_MSB, &msb, 1) || !i2cuRead(VEML6070_ADDR_CMD, &lsb, 1)) {
    s_sink.clear("lt_veml6070_uv");
    return;
  }
  uint16_t raw = ((uint16_t)msb << 8) | lsb;
  if (raw == 0xFFFF) {
    s_sink.clear("lt_veml6070_uv");
    return;
  }
  float k = config.extraF("veml6070_uv_scale", VEML6070_UV_SCALE);
  s_sink.publish("lt_veml6070_uv", (float)raw * k);
}

// VEML6075: kompensacja UVA/UVB światłem widzialnym i podczerwienią,
// potem indeks UV = (UVA' * respA + UVB' * respB) / 2
static void readVeml6075() {
  uint16_t uva = 0, uvb = 0, comp1 = 0, comp2 = 0;
  if (!r16le(VEML6075_ADDR, VEML6075_REG_UVA, uva) ||
      !r16le(VEML6075_ADDR, VEML6075_REG_UVB, uvb) ||
      !r16le(VEML6075_ADDR, VEML6075_REG_COMP1, comp1) ||
      !r16le(VEML6075_ADDR, VEML6075_REG_COMP2, comp2)) {
    s_sink.clear("lt_veml6075_uvi");
    s_sink.clear("lt_veml6075_uva");
    s_sink.clear("lt_veml6075_uvb");
    return;
  }
  float ca = (float)uva - VEML6075_UVA_A * (float)comp1 - VEML6075_UVA_B * (float)comp2;
  float cb = (float)uvb - VEML6075_UVB_C * (float)comp1 - VEML6075_UVB_D * (float)comp2;
  if (ca < 0.0f) ca = 0.0f;
  if (cb < 0.0f) cb = 0.0f;
  float respA = config.extraF("veml6075_uva_resp", 0.001111f);
  float respB = config.extraF("veml6075_uvb_resp", 0.00125f);
  s_sink.publish("lt_veml6075_uva", ca);
  s_sink.publish("lt_veml6075_uvb", cb);
  s_sink.publish("lt_veml6075_uvi", (ca * respA + cb * respB) / 2.0f);
}

// SI1145: rejestry pomiarowe są big-endian
static void readSi1145() {
  uint16_t vis = 0, ir = 0, uv = 0;
  if (!r16be(SI1145_ADDR, SI1145_REG_VIS, vis) ||
      !r16be(SI1145_ADDR, SI1145_REG_IR, ir) ||
      !r16be(SI1145_ADDR, SI1145_REG_UV, uv)) {
    s_sink.clear("lt_si1145_uv");
    s_sink.clear("lt_si1145_vis");
    s_sink.clear("lt_si1145_ir");
    return;
  }
  float k = config.extraF("si1145_uv_scale", 0.01f);
  s_sink.publish("lt_si1145_vis", (float)vis);
  s_sink.publish("lt_si1145_ir", (float)ir);
  s_sink.publish("lt_si1145_uv", (float)uv * k);
}

// INA219: napięcie magistrali 4 mV/LSB, prąd wg I_LSB, moc 20 * I_LSB
static void readIna219() {
  uint16_t bus = 0, cur = 0, pwr = 0;
  if (!r16be(s_inaAddr, INA_REG_BUS_VOLTAGE, bus) ||
      !r16be(s_inaAddr, INA_REG_CURRENT, cur) ||
      !r16be(s_inaAddr, INA_REG_POWER, pwr)) {
    s_sink.clear("pw_ina219_v");
    s_sink.clear("pw_ina219_i");
    s_sink.clear("pw_ina219_p");
    return;
  }
  float ilsb = config.extraF("ina219_ilsb", 0.0001f);
  s_sink.publish("pw_ina219_v", (float)(bus >> 3) * 0.004f);
  s_sink.publish("pw_ina219_i", (float)(int16_t)cur * ilsb);
  s_sink.publish("pw_ina219_p", (float)pwr * 20.0f * ilsb);
}

// INA226 (oraz INA228 - ten sam zestaw kanałów): 1,25 mV/LSB napięcia
static void readIna226() {
  uint16_t bus = 0, cur = 0, pwr = 0;
  if (!r16be(s_inaAddr, INA_REG_BUS_VOLTAGE, bus) ||
      !r16be(s_inaAddr, INA_REG_CURRENT, cur) ||
      !r16be(s_inaAddr, INA_REG_POWER, pwr)) {
    s_sink.clear("pw_ina226_v");
    s_sink.clear("pw_ina226_i");
    s_sink.clear("pw_ina226_p");
    return;
  }
  float ilsb = config.extraF("ina226_ilsb", 0.001f);
  s_sink.publish("pw_ina226_v", (float)(int16_t)bus * 0.00125f);
  s_sink.publish("pw_ina226_i", (float)(int16_t)cur * ilsb);
  s_sink.publish("pw_ina226_p", (float)pwr * 25.0f * ilsb);
}

// INA228: VSHUNT/VBUS 24-bitowe (VSHUNT ze znakiem), moc liczona z iloczynu
static void readIna228() {
  uint32_t shunt = 0, bus = 0;
  if (!r24be(s_inaAddr, INA228_REG_VSHUNT, shunt) ||
      !r24be(s_inaAddr, INA228_REG_VBUS, bus)) {
    s_sink.clear("pw_ina226_v");
    s_sink.clear("pw_ina226_i");
    s_sink.clear("pw_ina226_p");
    return;
  }
  int32_t sShunt = (shunt & 0x800000UL) ? (int32_t)(shunt | 0xFF000000UL) : (int32_t)shunt;
  float rsh = config.extraF("ina228_rshunt", 0.1f);
  if (rsh <= 0.0f) rsh = 0.1f;
  float v = (float)bus * INA228_VBUS_LSB;
  float i = (float)sShunt * s_ina228VshuntLsb / rsh;
  s_sink.publish("pw_ina226_v", v);
  s_sink.publish("pw_ina226_i", i);
  s_sink.publish("pw_ina226_p", v * i);
}

// INA3221: 3 kanały; napięcie 8 mV/LSB, prąd 40 uV/LSB / R_shunt
static void readIna3221() {
  uint16_t bus[3] = { 0, 0, 0 };
  uint16_t shunt[3] = { 0, 0, 0 };
  for (uint8_t k = 0; k < 3; k++) {
    if (!r16be(s_inaAddr, (uint8_t)(INA_REG_BUS_VOLTAGE + 2 * k), bus[k]) ||
        !r16be(s_inaAddr, (uint8_t)(0x01 + 2 * k), shunt[k])) {
      for (uint8_t j = 0; j < 3; j++) {
        s_sink.clear(INA3221_VID[j]);
        s_sink.clear(INA3221_IID[j]);
      }
      return;
    }
  }
  float rsh = config.extraF("ina3221_rshunt", 0.1f);
  if (rsh <= 0.0f) rsh = 0.1f;
  for (uint8_t k = 0; k < 3; k++) {
    float v = (float)((int16_t)bus[k] >> 3) * 0.008f;
    float i = (float)((int16_t)shunt[k] >> 3) * 40.0e-6f / rsh;
    s_sink.publish(INA3221_VID[k], v);
    s_sink.publish(INA3221_IID[k], i);
  }
}

// MAX17048: SoC 1/256 %, napięcie ogniwa 78,125 uV/LSB
static void readMax17048() {
  uint16_t soc = 0, vcell = 0;
  if (!readWithCrc(MAX17048_ADDR, MAX17048_REG_SOC, false, soc) ||
      !readWithCrc(MAX17048_ADDR, MAX17048_REG_VCELL, false, vcell)) {
    s_sink.clear("pw_bat_soc");
    s_sink.clear("pw_bat_v");
    return;
  }
  float socPct = (float)soc / 256.0f;
  if (socPct > 100.0f) socPct = 100.0f;
  s_sink.publish("pw_bat_soc", socPct);
  s_sink.publish("pw_bat_v", (float)vcell * MAX17048_VCELL_LSB);
}

// LC709203F: RSOC w 0,1 %, napięcie ogniwa w mV
static void readLc709203() {
  uint16_t rsoc = 0, mv = 0;
  if (!readWithCrc(LC709203F_ADDR, LC709203F_REG_RSOC, true, rsoc) ||
      !readWithCrc(LC709203F_ADDR, LC709203F_REG_VOLTAGE, true, mv)) {
    s_sink.clear("pw_lc709203_soc");
    s_sink.clear("pw_lc709203_v");
    return;
  }
  float soc = (float)rsoc / 10.0f;
  if (soc > 100.0f) soc = 100.0f;
  s_sink.publish("pw_lc709203_soc", soc);
  s_sink.publish("pw_lc709203_v", (float)mv / 1000.0f);
}

// =============================================================
//  Interfejs modułu
// =============================================================

static void beginImpl(const DrvSink& s) {
  s_sink = s;

  // światło (kolejność ważna - rozstrzyga konflikt adresu 0x29)
  detectOpt3001();
  detectTsl2591();
  detectTsl2561();
  detectLtr329();
  // UV
  detectVeml6070();
  detectVeml6075();
  detectSi1145();
  // zasilanie
  detectIna();
  detectMax17048();
  detectLc709203();
}

static void readImpl() {
  if (s_optOk)     readOpt3001();
  if (s_tsl2591Ok) readTsl2591();
  if (s_tsl2561Ok) readTsl2561();
  if (s_ltrOk)     readLtr329();
  if (s_veml6070Ok) readVeml6070();
  if (s_veml6075Ok) readVeml6075();
  if (s_si1145Ok)  readSi1145();

  if (s_inaKind == INA_KIND_219)       readIna219();
  else if (s_inaKind == INA_KIND_226)  readIna226();
  else if (s_inaKind == INA_KIND_228)  readIna228();
  else if (s_inaKind == INA_KIND_3221) readIna3221();

  if (s_maxOk) readMax17048();
  if (s_lcOk)  readLc709203();
}

const DrvModule drvLight = { "Swiatlo/Zasilanie", CHANS, NCHANS, beginImpl, readImpl };
