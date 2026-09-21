/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_motion.h"

#include <math.h>
#include <string.h>

#include <Wire.h>

#include "config.h"
#include "i2c_util.h"
#include "syslog.h"

// =============================================================
//  Czujniki ruchu, magnetometry, dalmierz laserowy i termometr IR
//  (I2C, ręcznie - bez bibliotek Adafruit_*/SparkFun_*, tylko Wire).
//
//  Obsługiwane układy:
//    * QMC5883L         0x0D       magnetometr 3 osie, tryb ciągły 50 Hz
//    * HMC5883L         0x1E       magnetometr 3 osie
//    * MPU6050          0x68/0x69  WHO_AM_I 0x68  - acc + gyro + T wewn.
//    * MPU6500          0x68/0x69  WHO_AM_I 0x70
//    * MPU9250/MPU9255  0x68/0x69  WHO_AM_I 0x71/0x73 (+ AK8963 0x0C za bypass I2C)
//    * LIS3DH           0x18/0x19  akcelerometr 3 osie
//    * ADXL345          0x53/0x1D  akcelerometr 3 osie
//    * VL53L0X/VL53L1X  0x29       dalmierz ToF (rozróżniany po rejestrze modelu)
//    * MLX90614         0x5A       bezdotykowy termometr IR (obiekt + otoczenie)
//
//  Wykrywanie jest konserwatywne: bez potwierdzenia (WHO_AM_I, rejestr
//  identyfikacji, PEC + sensowny zakres) układ nie jest zgłaszany, a jego
//  kanały milczą. Konflikty adresów rozstrzygamy jak w drv_light.cpp -
//  wygrywa PIERWSZY potwierdzony układ, kolejne są pomijane z LOG_I:
//    * 0x68 - w projekcie siedzi tam RTC (DS3231/DS1307, drv_rtc.cpp),
//      dlatego MPU musi oddać WHO_AM_I z rodziny i przejść kontrolny
//      odczyt PWR_MGMT_1 po resecie,
//    * 0x29 - TSL2591/TSL2561/LTR-329 (drv_light.cpp), więc VL53 musi
//      podać model 0xEE (L0X) albo 0xEA (L1X),
//    * 0x5A - CCS811 (drv_air.cpp), więc MLX90614 musi mieć poprawne PEC
//      i temperatury w sensownych zakresach,
//    * 0x18/0x19/0x1D - MCP9808 (drv_ths.cpp), więc LIS3DH/ADXL345 muszą
//      podać poprawne WHO_AM_I/DEVID i potwierdzić zapis konfiguracji.
//
//  Akcelerometry i magnetometry dzielą te same kanały (mot_acc_*,
//  mot_mag_*), więc działa tylko pierwszy potwierdzony układ danego typu -
//  reszta jest pomijana bez zapisu na magistrali.
//
//  read() nie blokuje: dalmierz ToF pracuje jako maszyna stanów (start
//  pomiaru w jednym wywołaniu, odczyt wyniku w kolejnym, bez delay()).
//  Publikowane są wyłącznie wartości surowe (offset dolicza SensorManager);
//  jedyne odstępstwa to sprzętowy offset dalmierza (klucz "tof_offset_mm"
//  wpisywany do rejestru VL53), deklinacja kursu ("mag_decl") i korekta
//  emisyjności MLX ("mlx_emiss").
// =============================================================

// ---------- Kanały ----------
enum {
  CH_MAG_X = 0, CH_MAG_Y, CH_MAG_Z, CH_HEADING,
  CH_IMU_T,
  CH_ACC_X, CH_ACC_Y, CH_ACC_Z,
  CH_GYRO_X, CH_GYRO_Y, CH_GYRO_Z,
  CH_ROLL, CH_PITCH, CH_VIB,
  CH_TOF_DIST, CH_TOF_L1_DIST, CH_TOF_AMB,
  CH_IR_OBJ, CH_IR_AMB
};

static const ChanDef CHANS[] = {
  { "mot_mag_x",     "Pole magn. X",             "µT",  1, "out", "",              "µT",  "mdi:magnet" },
  { "mot_mag_y",     "Pole magn. Y",             "µT",  1, "out", "",              "µT",  "mdi:magnet" },
  { "mot_mag_z",     "Pole magn. Z",             "µT",  1, "out", "",              "µT",  "mdi:magnet" },
  { "mot_heading",   "Kurs (magnetometr)",       "°",   0, "out", "",              "°",   "mdi:compass" },
  { "mot_imu_t",     "Temperatura IMU",          "°C",  1, "in",  "temperature",   "°C",  "mdi:thermometer" },
  { "mot_acc_x",     "Przyspieszenie X",         "g",   3, "in",  "",              "g",   "mdi:axis-x-arrow" },
  { "mot_acc_y",     "Przyspieszenie Y",         "g",   3, "in",  "",              "g",   "mdi:axis-y-arrow" },
  { "mot_acc_z",     "Przyspieszenie Z",         "g",   3, "in",  "",              "g",   "mdi:axis-z-arrow" },
  { "mot_gyro_x",    "Żyroskop X",               "°/s", 1, "in",  "",              "°/s", "mdi:rotate-3d-variant" },
  { "mot_gyro_y",    "Żyroskop Y",               "°/s", 1, "in",  "",              "°/s", "mdi:rotate-3d-variant" },
  { "mot_gyro_z",    "Żyroskop Z",               "°/s", 1, "in",  "",              "°/s", "mdi:rotate-3d-variant" },
  { "mot_roll",      "Przechył (roll)",          "°",   1, "in",  "",              "°",   "mdi:angle-acute" },
  { "mot_pitch",     "Pochylenie (pitch)",       "°",   1, "in",  "",              "°",   "mdi:angle-obtuse" },
  { "mot_vib",       "Drgania",                  "g",   3, "in",  "",              "g",   "mdi:vibrate" },
  { "mot_tof_dist",  "Dalmierz (VL53L0X)",       "mm",  0, "out", "distance",      "mm",  "mdi:ruler" },
  { "mot_tof_l1_dist", "Dalmierz (VL53L1X)",     "mm",  0, "out", "distance",      "mm",  "mdi:ruler" },
  { "mot_tof_amb",   "Światło otoczenia (ToF)",  "",    0, "out", "",              "",    "mdi:brightness-5" },
  { "mot_ir_obj_t",  "Temperatura obiektu (IR)", "°C",  1, "in",  "temperature",   "°C",  "mdi:thermometer-infrared" },
  { "mot_ir_amb_t",  "Temperatura otoczenia (IR)", "°C", 1, "in", "temperature",   "°C",  "mdi:thermometer" }
};

static const uint16_t NCHANS = sizeof(CHANS) / sizeof(CHANS[0]);

// ---------- Bramki wartości (publikujemy tylko rozsądne pomiary) ----------
#define MOT_MAG_MIN      (-1200.0f)   // µT
#define MOT_MAG_MAX      (1200.0f)
#define MOT_IMU_T_MIN    (-40.0f)     // °C (temperatura wewnętrzna IMU)
#define MOT_IMU_T_MAX    (125.0f)
#define MOT_ACC_MAX      (16.0f)      // g
#define MOT_GYRO_MAX     (2000.0f)    // °/s
#define MOT_VIB_MAX      (16.0f)      // g
#define MOT_TOF_MM_MAX   (8000.0f)    // mm
#define MOT_TOF_AMB_MAX  (65535.0f)   // MCPS (część całkowita tempa otoczenia, format 16.16)
#define MOT_IR_TO_MIN    (-70.0f)     // °C (obiekt)
#define MOT_IR_TO_MAX    (380.0f)
#define MOT_IR_TA_MIN    (-40.0f)     // °C (otoczenie)
#define MOT_IR_TA_MAX    (125.0f)
#define MOT_PI           3.14159265f

// ---------- Adresy ----------
#define MOT_QMC_ADDR     0x0D
#define MOT_HMC_ADDR     0x1E
#define MOT_AK_ADDR      0x0C
#define MOT_MPU_ADDR_A   0x68
#define MOT_MPU_ADDR_B   0x69
#define MOT_LIS_ADDR_A   0x18
#define MOT_LIS_ADDR_B   0x19
#define MOT_ADXL_ADDR_A  0x53
#define MOT_ADXL_ADDR_B  0x1D
#define MOT_TOF_ADDR     0x29
#define MOT_MLX_ADDR     0x5A

// ---------- QMC5883L (0x0D) ----------
#define QMC_REG_DATA     0x00    // X LSB/MSB, Y LSB/MSB, Z LSB/MSB, status
#define QMC_REG_STATUS   0x06    // bit0 = DRDY
#define QMC_REG_CTRL1    0x09
#define QMC_REG_CTRL2    0x0A
#define QMC_REG_SETRESET 0x0B
#define QMC_REG_CHIP_ID  0x0D
#define QMC_ID           0xFF
// MODE=01 (tryb ciągły), OSR=00 (512), RNG=00 (±2 G, 12000 LSB/G), ODR=01 (50 Hz)
#define QMC_CTRL1_CONT   0x41

// ---------- HMC5883L (0x1E) ----------
#define HMC_REG_CFG_A    0x00
#define HMC_REG_CFG_B    0x01
#define HMC_REG_MODE     0x02
#define HMC_REG_DATA     0x03    // X MSB/LSB, Z MSB/LSB, Y MSB/LSB
#define HMC_REG_STATUS   0x09    // bit0 = RDY
#define HMC_REG_ID_A     0x0A    // "H43"
#define HMC_CFG_A_15HZ   0x70    // 8 średnich, 15 Hz, normalny pomiar
#define HMC_CFG_B_1090   0x20    // wzmocnienie 1090 LSB/G (±1,3 G)

// ---------- MPU6050/6500/9250/9255 (0x68/0x69) ----------
#define MPU_REG_SMPLRT   0x19
#define MPU_REG_CONFIG   0x1A
#define MPU_REG_GYRO_CFG 0x1B
#define MPU_REG_ACC_CFG  0x1C
#define MPU_REG_ACC_CFG2 0x1D
#define MPU_REG_INT_PIN  0x37    // bit1 = I2C_BYPASS_EN (dostęp do AK8963)
#define MPU_REG_INT_EN   0x38
#define MPU_REG_ACC_OUT  0x3B    // 14 bajtów: acc, temp, gyro
#define MPU_REG_PWR1     0x6B
#define MPU_REG_PWR2     0x6C
#define MPU_REG_WHO_AM_I 0x75

// ---------- AK8963 (0x0C, za bypassem MPU9250/MPU9255) ----------
#define AK_REG_WIA       0x00    // 0x48
#define AK_REG_ST1       0x02    // bit0 = DRDY, bit1 = DOR
#define AK_REG_DATA      0x03    // X LSB/MSB, Y LSB/MSB, Z LSB/MSB
#define AK_REG_ST2       0x09
#define AK_REG_CNTL1     0x0A
#define AK_REG_CNTL2     0x0B
#define AK_ID            0x48
#define AK_CNTL1_16BIT   0x16    // 16 bit, tryb ciągły 2 (100 Hz)

// ---------- LIS3DH (0x18/0x19) ----------
#define LIS_REG_WHO_AM_I 0x0F    // 0x33
#define LIS_REG_CTRL1    0x20
#define LIS_REG_CTRL4    0x23    // bit7 = BDU, bity 5:4 = zakres
#define LIS_REG_STATUS   0x27    // bit3 = ZYXDA
#define LIS_REG_OUT_X_L  0x28
#define LIS_ID           0x33
#define LIS_CTRL1_100HZ  0x57    // ODR=100 Hz, XYZ włączone, tryb normalny

// ---------- ADXL345 (0x53/0x1D) ----------
#define ADXL_REG_DEVID   0x00    // 0xE5
#define ADXL_REG_BW_RATE 0x2C
#define ADXL_REG_POWER   0x2D    // bit3 = measure
#define ADXL_REG_DATAFMT 0x31    // bit3 = FULL_RES, bity 1:0 = zakres
#define ADXL_REG_DATA    0x32
#define ADXL_ID          0xE5
#define ADXL_BW_100HZ    0x0A
#define ADXL_POWER_ON    0x08
#define ADXL_FULL_RES    0x08
#define ADXL_LSB_PER_G   256.0f  // FULL_RES: 3,9 mg/LSB

// ---------- VL53L0X (0x29) ----------
#define L0X_REG_SYSRANGE_START   0x00
#define L0X_REG_SEQ_CONFIG       0x01
#define L0X_REG_INT_GPIO_CONFIG  0x0A
#define L0X_REG_INT_CLEAR        0x0B
#define L0X_REG_RESULT_INT_STAT  0x13    // bity 2:0 = nowy pomiar gotowy
#define L0X_REG_RESULT_STATUS    0x14    // blok 12 bajtów
#define L0X_REG_OFFSET           0x28    // 1/16 mm (int16)
#define L0X_REG_SIGNAL_LIMIT     0x44    // limit tempa sygnału 16.16
#define L0X_REG_MSRC_CONFIG      0x60
#define L0X_REG_VHV_CONFIG       0x89
#define L0X_REG_MODEL_ID         0xC0    // 0xEE
#define L0X_ID                   0xEE
#define L0X_STATUS_VALID         11      // kod "Range Valid" w rejestrze statusu
#define L0X_SEQ_CONTINUOUS       0xE8    // domyślna sekwencja pomiaru

// ---------- VL53L1X (0x29) ----------
#define L1X_REG_MODEL_ID       0x010F    // 0xEA
#define L1X_REG_OFFSET         0x001E    // 1/16 mm (int16)
#define L1X_REG_INT_CLEAR      0x0086
#define L1X_REG_MODE_START     0x0087    // 0x40 = pomiar jednorazowy
#define L1X_REG_RESULT_STATUS  0x0089    // blok 17 bajtów (0x0089..0x0099)
#define L1X_ID                 0xEA

// ---------- MLX90614 (0x5A) ----------
#define MLX_REG_TA       0x06    // temperatura otoczenia
#define MLX_REG_TOBJ     0x07    // temperatura obiektu
#define MLX_REG_EMISS    0x24    // emisyjność z EEPROM (tylko odczyt)
#define MLX_ADDR_W       0xB4    // 0x5A << 1 (adres z bitem zapisu - do PEC)
#define MLX_ADDR_R       0xB5    // 0x5A << 1 | 1 (adres z bitem odczytu - do PEC)

// ---------- Odstępy / limity czasu ----------
#define MOT_TOF_TIMEOUT_MS  6000   // read() chodzi co ~5 s, więc wynik musi się zmieścić
#define MOT_TOF_MIN_WAIT_MS 200    // minimalny czas pomiaru VL53L1X przed odczytem

// ---------- Zasilanie / stan modułu ----------
static DrvSink s_sink;             // kopia sinku przekazanego w begin()

// Rodzaje magnetometru
enum { MAG_NONE = 0, MAG_QMC, MAG_HMC, MAG_AK };
// Rodzaje dalmierza
enum { TOF_NONE = 0, TOF_L0X, TOF_L1X };

struct MotionDev {
  bool     magOk;
  uint8_t  magKind;          // MAG_*
  uint8_t  magAddr;
  float    magScale;         // µT na LSB
  bool     mpuOk;            // MPU6050/6500/9250/9255
  uint8_t  mpuAddr;
  uint8_t  mpuWho;           // WHO_AM_I
  float    accScale;         // g na LSB
  float    gyroScale;        // °/s na LSB
  float    imuTScale;        // °C na LSB
  float    imuTOff;          // °C
  bool     lisOk;            // LIS3DH
  uint8_t  lisAddr;
  float    lisScale;         // g na LSB
  bool     adxlOk;           // ADXL345
  uint8_t  adxlAddr;
  bool     tofOk;
  uint8_t  tofKind;          // TOF_*
  bool     tofRun;           // pomiar w toku
  uint32_t tofMs;            // znacznik startu pomiaru
  bool     mlxOk;            // MLX90614
  float    mlxEmissEeprom;   // emisyjność z EEPROM układu
  bool     warnMag, warnImu, warnTof, warnIr;   // LOG_W tylko raz na epizod
  uint8_t  accFsIdx;         // indeks zakresu ±2/4/8/16 g
  uint8_t  gyroFsIdx;        // indeks zakresu ±250/500/1000/2000 °/s
  float    decl;             // deklinacja z configu "mag_decl" [°]
  float    mlxEmiss;         // emisyjność z configu "mlx_emiss"
  float    tofOffsetMm;      // offset dalmierza z configu "tof_offset_mm" [mm]
};

static MotionDev s_dev;

// ---------- Tabele zakresów ----------
static const float MOT_ACC_FS[4]      = { 2.0f, 4.0f, 8.0f, 16.0f };
static const float MOT_GYRO_FS[4]     = { 250.0f, 500.0f, 1000.0f, 2000.0f };
static const float MOT_MPU_ACC_LSB[4] = { 16384.0f, 8192.0f, 4096.0f, 2048.0f };  // LSB/g
static const float MOT_MPU_GYR_LSB[4] = { 131.0f, 65.5f, 32.8f, 16.4f };         // LSB/(°/s)
static const float MOT_LIS_ACC_LSB[4] = { 1000.0f, 500.0f, 250.0f, 83.333f };    // LSB/g

// ---------- Pomocniki publikacji ----------
static void publishCh(uint8_t ch, float v) {
  if (!s_sink.publish) return;
  s_sink.publish(CHANS[ch].id, v);
}

static void clearCh(uint8_t ch) {
  if (!s_sink.clear) return;
  s_sink.clear(CHANS[ch].id);
}

static void foundCh(uint8_t ch) {
  if (!s_sink.found) return;
  s_sink.found(CHANS[ch].id);
}

// Publikacja z kontrolą sensowności - poza zakresem czyścimy kanał
static void pubGated(uint8_t ch, float v, float lo, float hi) {
  if (isnan(v) || !(v >= lo && v <= hi)) { clearCh(ch); return; }
  publishCh(ch, v);
}

// ---------- Pomocniki liczbowe ----------
static int16_t leS16(const uint8_t* b) {
  return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static int16_t beS16(const uint8_t* b) {
  return (int16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

// Indeks wartości najbliższej zadanej (zakresy IMU: 2/4/8/16 g itd.)
static uint8_t snapIdx(float v, const float* tab, uint8_t n) {
  uint8_t best = 0;
  float d = fabsf(v - tab[0]);
  for (uint8_t i = 1; i < n; i++) {
    float dd = fabsf(v - tab[i]);
    if (dd < d) { d = dd; best = i; }
  }
  return best;
}

// Kąty z akcelerometru (gdy nie ma żyroskopu wystarczają do przechyłu)
static void accAngles(float ax, float ay, float az, float& roll, float& pitch) {
  roll  = atan2f(ay, az) * 180.0f / MOT_PI;
  pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / MOT_PI;
}

// Intensywność drgań = odchylenie modułu przyspieszenia od 1 g
static float vibeOf(float ax, float ay, float az) {
  return fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);
}

// Kurs z magnetometru [°]: obrót wektora pola w płaszczyźnie XY
// (bez kompensacji przechyłu, z deklinacją z configu "mag_decl")
static float magHeading(float mx, float my) {
  float h = atan2f(my, mx) * 180.0f / MOT_PI;
  h += s_dev.decl;
  while (h >= 360.0f) h -= 360.0f;
  while (h < 0.0f)    h += 360.0f;
  return h;
}

// ---------- Czyszczenie kanałów poszczególnych grup ----------
static void clearMagChans() {
  clearCh(CH_MAG_X); clearCh(CH_MAG_Y); clearCh(CH_MAG_Z); clearCh(CH_HEADING);
}

static void clearAccChans() {
  clearCh(CH_ACC_X); clearCh(CH_ACC_Y); clearCh(CH_ACC_Z);
  clearCh(CH_ROLL);  clearCh(CH_PITCH);  clearCh(CH_VIB);
}

static void clearImuChans() {
  clearAccChans();
  clearCh(CH_GYRO_X); clearCh(CH_GYRO_Y); clearCh(CH_GYRO_Z); clearCh(CH_IMU_T);
}

static void clearTofChans() {
  clearCh(CH_TOF_DIST); clearCh(CH_TOF_L1_DIST); clearCh(CH_TOF_AMB);
}

// =============================================================
//  QMC5883L (0x0D)
// =============================================================
static bool qmcInit() {
  // Rejestry konfiguracji + SET/RESET (wymagane do pracy magnetometru)
  if (!i2cuWriteReg(MOT_QMC_ADDR, QMC_REG_CTRL2, 0x00)) return false;
  if (!i2cuWriteReg(MOT_QMC_ADDR, QMC_REG_SETRESET, 0x01)) return false;
  if (!i2cuWriteReg(MOT_QMC_ADDR, QMC_REG_CTRL1, QMC_CTRL1_CONT)) return false;
  delay(10);
  // Potwierdzenie: konfiguracja i identyfikator muszą się odczytać
  uint8_t rb = 0, id = 0;
  if (!i2cuReadReg(MOT_QMC_ADDR, QMC_REG_CTRL1, &rb, 1) || rb != QMC_CTRL1_CONT) return false;
  if (!i2cuReadReg(MOT_QMC_ADDR, QMC_REG_CHIP_ID, &id, 1) || id != QMC_ID) return false;
  return true;
}

// =============================================================
//  HMC5883L (0x1E)
// =============================================================
static bool hmcIdOk() {
  uint8_t id[3];
  if (!i2cuReadReg(MOT_HMC_ADDR, HMC_REG_ID_A, id, 3)) return false;
  return id[0] == 'H' && id[1] == '4' && id[2] == '3';
}

static bool hmcInit() {
  if (!i2cuWriteReg(MOT_HMC_ADDR, HMC_REG_CFG_A, HMC_CFG_A_15HZ)) return false;
  if (!i2cuWriteReg(MOT_HMC_ADDR, HMC_REG_CFG_B, HMC_CFG_B_1090)) return false;
  if (!i2cuWriteReg(MOT_HMC_ADDR, HMC_REG_MODE, 0x00)) return false;   // tryb ciągły
  delay(10);
  uint8_t rb = 0;
  if (!i2cuReadReg(MOT_HMC_ADDR, HMC_REG_CFG_A, &rb, 1) || rb != HMC_CFG_A_15HZ) return false;
  return hmcIdOk();
}

// =============================================================
//  MPU6050 / MPU6500 / MPU9250 / MPU9255 (0x68/0x69)
// =============================================================
static bool mpuInit(uint8_t a, uint8_t who) {
  // Reset (PWR_MGMT_1 = 0x80), potem wybudzenie z zegarem z żyroskopu.
  // delay() jest tu bezpieczny - to begin(), nie read().
  if (!i2cuWriteReg(a, MPU_REG_PWR1, 0x80)) return false;
  delay(50);
  uint8_t rb = 0;
  if (!i2cuReadReg(a, MPU_REG_WHO_AM_I, &rb, 1) || rb != who) return false;
  if (!i2cuWriteReg(a, MPU_REG_PWR1, 0x01)) return false;
  delay(10);
  if (!i2cuReadReg(a, MPU_REG_PWR1, &rb, 1)) return false;
  if ((rb & 0x40) != 0) return false;      // SLEEP nie ustąpił
  if ((rb & 0x07) == 0) return false;      // brak wybranego źródła zegara

  const uint8_t accCfg  = (uint8_t)(s_dev.accFsIdx << 3);
  const uint8_t gyroCfg = (uint8_t)(s_dev.gyroFsIdx << 3);
  i2cuWriteReg(a, MPU_REG_SMPLRT, 0x04);       // 1 kHz / (1+4) = 200 Hz
  i2cuWriteReg(a, MPU_REG_CONFIG, 0x03);       // DLPF: żyroskop 44 Hz / acc 42 Hz
  i2cuWriteReg(a, MPU_REG_GYRO_CFG, gyroCfg);
  i2cuWriteReg(a, MPU_REG_ACC_CFG, accCfg);
  if (who != 0x68) i2cuWriteReg(a, MPU_REG_ACC_CFG2, 0x03);   // tylko MPU6500+ ma ten rejestr
  i2cuWriteReg(a, MPU_REG_PWR2, 0x00);         // wszystkie osie włączone
  i2cuWriteReg(a, MPU_REG_INT_PIN, 0x02);      // I2C_BYPASS_EN - dostęp do AK8963 (MPU9250)
  i2cuWriteReg(a, MPU_REG_INT_EN, 0x00);       // bez przerwań
  delay(5);

  // Potwierdzenie: to, co zapisaliśmy, musi się odczytać
  uint8_t ra = 0, rg = 0;
  if (!i2cuReadReg(a, MPU_REG_ACC_CFG, &ra, 1) || ra != accCfg) return false;
  if (!i2cuReadReg(a, MPU_REG_GYRO_CFG, &rg, 1) || rg != gyroCfg) return false;

  s_dev.accScale  = 1.0f / MOT_MPU_ACC_LSB[s_dev.accFsIdx];
  s_dev.gyroScale = 1.0f / MOT_MPU_GYR_LSB[s_dev.gyroFsIdx];
  if (who == 0x68) {                       // MPU6050 - inny przelicznik temperatury
    s_dev.imuTScale = 1.0f / 340.0f;
    s_dev.imuTOff   = 36.53f;
  } else {
    s_dev.imuTScale = 1.0f / 333.87f;
    s_dev.imuTOff   = 21.0f;
  }
  return true;
}

// =============================================================
//  AK8963 (0x0C) - magnetometr w MPU9250/MPU9255 (przez bypass I2C)
// =============================================================
static bool akIdOk() {
  uint8_t id = 0;
  return i2cuReadReg(MOT_AK_ADDR, AK_REG_WIA, &id, 1) && id == AK_ID;
}

static bool akInit() {
  i2cuWriteReg(MOT_AK_ADDR, AK_REG_CNTL2, 0x01);   // miękki reset
  delay(10);
  i2cuWriteReg(MOT_AK_ADDR, AK_REG_CNTL1, 0x00);   // tryb power-down przed zmianą
  delay(10);
  if (!i2cuWriteReg(MOT_AK_ADDR, AK_REG_CNTL1, AK_CNTL1_16BIT)) return false;
  delay(10);
  uint8_t rb = 0;
  if (!i2cuReadReg(MOT_AK_ADDR, AK_REG_CNTL1, &rb, 1) || rb != AK_CNTL1_16BIT) return false;
  return akIdOk();
}

// =============================================================
//  LIS3DH (0x18/0x19)
// =============================================================
static bool lisInit(uint8_t a) {
  const uint8_t ctrl4 = (uint8_t)(0x08 | (s_dev.accFsIdx << 4));   // BDU + zakres
  if (!i2cuWriteReg(a, LIS_REG_CTRL1, LIS_CTRL1_100HZ)) return false;
  if (!i2cuWriteReg(a, LIS_REG_CTRL4, ctrl4)) return false;
  delay(5);
  uint8_t r1 = 0, r4 = 0;
  if (!i2cuReadReg(a, LIS_REG_CTRL1, &r1, 1) || r1 != LIS_CTRL1_100HZ) return false;
  if (!i2cuReadReg(a, LIS_REG_CTRL4, &r4, 1) || r4 != ctrl4) return false;
  s_dev.lisScale = 1.0f / MOT_LIS_ACC_LSB[s_dev.accFsIdx];
  return true;
}

// =============================================================
//  ADXL345 (0x53/0x1D)
// =============================================================
static bool adxlInit(uint8_t a) {
  const uint8_t fmt = (uint8_t)(ADXL_FULL_RES | s_dev.accFsIdx);   // FULL_RES = 3,9 mg/LSB
  if (!i2cuWriteReg(a, ADXL_REG_BW_RATE, ADXL_BW_100HZ)) return false;
  if (!i2cuWriteReg(a, ADXL_REG_DATAFMT, fmt)) return false;
  if (!i2cuWriteReg(a, ADXL_REG_POWER, ADXL_POWER_ON)) return false;
  delay(5);
  uint8_t rf = 0, rp = 0;
  if (!i2cuReadReg(a, ADXL_REG_DATAFMT, &rf, 1) || rf != fmt) return false;
  if (!i2cuReadReg(a, ADXL_REG_POWER, &rp, 1) || rp != ADXL_POWER_ON) return false;
  return true;
}

// =============================================================
//  VL53L0X (0x29)
// =============================================================
static bool l0xInit() {
  uint8_t id = 0;
  if (!i2cuReadReg(MOT_TOF_ADDR, L0X_REG_MODEL_ID, &id, 1) || id != L0X_ID) return false;
  if (!i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_MSRC_CONFIG, 0x12)) return false;
  // VHV: włącz wewnętrzny generator wysokiego napięcia, jeśli wyłączony
  uint8_t vhv = 0;
  if (i2cuReadReg(MOT_TOF_ADDR, L0X_REG_VHV_CONFIG, &vhv, 1) && (vhv & 0x01) == 0) {
    i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_VHV_CONFIG, (uint8_t)(vhv | 0x01));
  }
  if (!i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_INT_GPIO_CONFIG, 0x04)) return false;   // przerwanie: nowy pomiar
  if (!i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_INT_CLEAR, 0x01)) return false;
  if (!i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_SEQ_CONFIG, L0X_SEQ_CONTINUOUS)) return false;
  // Limit tempa sygnału 0,25 MCPS w formacie 16.16
  i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_SIGNAL_LIMIT, 0x00);
  i2cuWriteReg(MOT_TOF_ADDR, (uint8_t)(L0X_REG_SIGNAL_LIMIT + 1), 0x40);
  i2cuWriteReg(MOT_TOF_ADDR, (uint8_t)(L0X_REG_SIGNAL_LIMIT + 2), 0x00);
  i2cuWriteReg(MOT_TOF_ADDR, (uint8_t)(L0X_REG_SIGNAL_LIMIT + 3), 0x00);
  // Offset sprzętowy [1/16 mm] z konfiguracji - zapis do układu, nie do wartości
  if (s_dev.tofOffsetMm != 0.0f) {
    int32_t off16 = (int32_t)(s_dev.tofOffsetMm * 16.0f);
    if (off16 > 32767) off16 = 32767;
    if (off16 < -32768) off16 = -32768;
    i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_OFFSET, (uint8_t)((off16 >> 8) & 0xFF));
    i2cuWriteReg(MOT_TOF_ADDR, (uint8_t)(L0X_REG_OFFSET + 1), (uint8_t)(off16 & 0xFF));
  }
  delay(10);
  // Potwierdzenie: identyfikator + konfiguracja muszą wrócić tak samo
  uint8_t rb = 0;
  if (!i2cuReadReg(MOT_TOF_ADDR, L0X_REG_MODEL_ID, &rb, 1) || rb != L0X_ID) return false;
  if (!i2cuReadReg(MOT_TOF_ADDR, L0X_REG_SEQ_CONFIG, &rb, 1) || rb != L0X_SEQ_CONTINUOUS) return false;
  if (!i2cuReadReg(MOT_TOF_ADDR, L0X_REG_INT_GPIO_CONFIG, &rb, 1) || rb != 0x04) return false;
  return true;
}

// Start pomiaru jednorazowego VL53L0X
static bool l0xStart() {
  i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_INT_CLEAR, 0x01);
  return i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_SYSRANGE_START, 0x01);
}

// =============================================================
//  VL53L1X (0x29)
// =============================================================
// Zapis 1 bajtu pod 16-bitowy indeks (i2c_util.h nie ma takiego wariantu)
static bool l1xWrite8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(MOT_TOF_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool l1xReadBlock(uint16_t reg, uint8_t* buf, size_t len) {
  return i2cuWrite16(MOT_TOF_ADDR, reg) && i2cuRead(MOT_TOF_ADDR, buf, len);
}

static bool l1xRead16(uint16_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!l1xReadBlock(reg, b, 2)) return false;
  v = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
  return true;
}

static bool l1xInit() {
  uint16_t model = 0;
  if (!l1xRead16(L1X_REG_MODEL_ID, model) || model != L1X_ID) return false;
  // Miękki reset i ponowne potwierdzenie modelu (układ musi wstać z firmware)
  l1xWrite8(0x0000, 0x00);
  delay(5);
  l1xWrite8(0x0000, 0x01);
  delay(10);
  if (!l1xRead16(L1X_REG_MODEL_ID, model) || model != L1X_ID) return false;
  // Offset sprzętowy [1/16 mm] z konfiguracji
  if (s_dev.tofOffsetMm != 0.0f) {
    int32_t off16 = (int32_t)(s_dev.tofOffsetMm * 16.0f);
    if (off16 > 32767) off16 = 32767;
    if (off16 < -32768) off16 = -32768;
    l1xWrite8(L1X_REG_OFFSET, (uint8_t)((off16 >> 8) & 0xFF));
    l1xWrite8((uint16_t)(L1X_REG_OFFSET + 1), (uint8_t)(off16 & 0xFF));
  }
  l1xWrite8(L1X_REG_INT_CLEAR, 0x01);
  return true;
}

static bool l1xStart() {
  l1xWrite8(L1X_REG_INT_CLEAR, 0x01);
  return l1xWrite8(L1X_REG_MODE_START, 0x40);   // pomiar jednorazowy
}

// =============================================================
//  MLX90614 (0x5A) - termometr IR, PEC wg noty Melexis
// =============================================================
// CRC-8 (wielomian 0x07, inicjalizacja 0x00) - inny niż Sensirion!
static uint8_t mlxCrc8(const uint8_t* d, size_t n) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Odczyt surowego słowa RAM/EEPROM; PEC musi się zgadzać
// (wariant z adresami: [0xB4, cmd, 0xB5, LSB, MSB] albo sam [LSB, MSB])
static bool mlxReadRaw(uint8_t cmd, uint16_t& raw) {
  uint8_t b[3];
  if (!i2cuReadReg(MOT_MLX_ADDR, cmd, b, 3)) return false;
  const uint8_t seqA[5] = { MLX_ADDR_W, cmd, MLX_ADDR_R, b[0], b[1] };
  if (mlxCrc8(seqA, sizeof(seqA)) != b[2] && mlxCrc8(b, 2) != b[2]) return false;
  raw = (uint16_t)(((uint16_t)b[1] << 8) | b[0]);   // MLX wysyła LSB jako pierwszy
  return true;
}

static float mlxRawToC(uint16_t raw) {
  return (float)(int16_t)raw * 0.02f - 273.15f;
}

// Korekta emisyjności: układowe To liczone jest dla emisyjności z EEPROM,
// więc przy innej wartości z configu przeliczamy: e_eep*Tm^4 = e_cfg*Tt^4 + (1-e_cfg)*Ta^4
static float mlxCorrect(float tobjC, float tambC, float eEeprom, float eCfg) {
  if (fabsf(eCfg - eEeprom) <= 0.02f) return tobjC;
  if (eEeprom < 0.05f || eCfg < 0.05f) return tobjC;
  const float TK = 273.15f;
  float tm = tobjC + TK;
  float ta = tambC + TK;
  float t4 = (eEeprom * powf(tm, 4.0f) - (1.0f - eCfg) * powf(ta, 4.0f)) / eCfg;
  if (isnan(t4) || t4 <= 0.0f) return tobjC;
  return powf(t4, 0.25f) - TK;
}

// =============================================================
//  Wykrywanie sprzętu (begin)
// =============================================================
// MPU9250 i MPU9255 mają ten sam WHO_AM_I co MPU6500 (0x71/0x73),
// więc rozpoznajemy je po odpowiedzi magnetometru AK8963 za bypassem I2C.
static const char* mpuVariantName(uint8_t who, bool akPresent) {
  if (akPresent) return "MPU9250/9255";
  switch (who) {
    case 0x68: return "MPU6050";
    case 0x70: return "MPU6500";
    case 0x74: return "MPU6500";
    default:   return "MPU6xxx";
  }
}

static void detectImu() {
  // --- MPU6050/6500/9250/9255 ---
  const uint8_t mpuAddrs[2] = { MOT_MPU_ADDR_A, MOT_MPU_ADDR_B };
  for (uint8_t i = 0; i < 2; i++) {
    const uint8_t a = mpuAddrs[i];
    if (!i2cuPresent(a)) continue;
    uint8_t who = 0;
    if (!i2cuReadReg(a, MPU_REG_WHO_AM_I, &who, 1)) continue;
    if (!(who == 0x68 || who == 0x70 || who == 0x71 || who == 0x73 || who == 0x74)) {
      // 0x68 w tej stacji należy zwykle do RTC (DS3231/DS1307) - nie ruszamy go
      LOG_I("MPU: pod 0x%02X nie ma układu z rodziny MPU (WHO_AM_I 0x%02X) - pomijam", a, who);
      continue;
    }
    if (s_dev.mpuOk) {
      LOG_I("MPU: pomijam kolejny układ pod adresem 0x%02X - obsługiwany jest pierwszy", a);
      continue;
    }
    if (!mpuInit(a, who)) {
      LOG_I("MPU: układ pod 0x%02X nie potwierdził konfiguracji (WHO_AM_I 0x%02X) - pomijam", a, who);
      continue;
    }
    s_dev.mpuOk   = true;
    s_dev.mpuAddr = a;
    s_dev.mpuWho  = who;
    // MPU9250/9255 mają WHO_AM_I jak MPU6500 - rozróżniamy po odpowiedzi AK8963
    const bool akPresent = akIdOk();
    LOG_I("IMU: %s pod 0x%02X (acc ±%.0f g, żyro ±%.0f °/s, temp. wewnętrzna)",
          mpuVariantName(who, akPresent), a,
          MOT_ACC_FS[s_dev.accFsIdx], MOT_GYRO_FS[s_dev.gyroFsIdx]);
    foundCh(CH_ACC_X); foundCh(CH_ACC_Y); foundCh(CH_ACC_Z);
    foundCh(CH_GYRO_X); foundCh(CH_GYRO_Y); foundCh(CH_GYRO_Z);
    foundCh(CH_ROLL); foundCh(CH_PITCH); foundCh(CH_VIB); foundCh(CH_IMU_T);
  }

  // --- LIS3DH (tylko jeśli nie ma już akcelerometru w IMU) ---
  const uint8_t lisAddrs[2] = { MOT_LIS_ADDR_A, MOT_LIS_ADDR_B };
  for (uint8_t i = 0; i < 2; i++) {
    const uint8_t a = lisAddrs[i];
    if (!i2cuPresent(a)) continue;
    uint8_t who = 0;
    if (!i2cuReadReg(a, LIS_REG_WHO_AM_I, &who, 1) || who != LIS_ID) continue;
    if (s_dev.mpuOk || s_dev.lisOk) {
      LOG_I("LIS3DH: pomijam akcelerometr pod adresem 0x%02X - obsługiwany jest pierwszy", a);
      continue;
    }
    if (!lisInit(a)) {
      LOG_I("LIS3DH: układ pod 0x%02X nie potwierdził konfiguracji - pomijam", a);
      continue;
    }
    s_dev.lisOk   = true;
    s_dev.lisAddr = a;
    LOG_I("LIS3DH: wykryty pod 0x%02X (akcelerometr ±%.0f g)", a, MOT_ACC_FS[s_dev.accFsIdx]);
    foundCh(CH_ACC_X); foundCh(CH_ACC_Y); foundCh(CH_ACC_Z);
    foundCh(CH_ROLL); foundCh(CH_PITCH); foundCh(CH_VIB);
  }

  // --- ADXL345 (0x53, awaryjnie 0x1D) ---
  const uint8_t adxlAddrs[2] = { MOT_ADXL_ADDR_A, MOT_ADXL_ADDR_B };
  for (uint8_t i = 0; i < 2; i++) {
    const uint8_t a = adxlAddrs[i];
    if (!i2cuPresent(a)) continue;
    uint8_t id = 0;
    if (!i2cuReadReg(a, ADXL_REG_DEVID, &id, 1) || id != ADXL_ID) continue;
    if (s_dev.mpuOk || s_dev.lisOk || s_dev.adxlOk) {
      LOG_I("ADXL345: pomijam akcelerometr pod adresem 0x%02X - obsługiwany jest pierwszy", a);
      continue;
    }
    if (!adxlInit(a)) {
      LOG_I("ADXL345: układ pod 0x%02X nie potwierdził konfiguracji - pomijam", a);
      continue;
    }
    s_dev.adxlOk   = true;
    s_dev.adxlAddr = a;
    LOG_I("ADXL345: wykryty pod 0x%02X (akcelerometr ±%.0f g)", a, MOT_ACC_FS[s_dev.accFsIdx]);
    foundCh(CH_ACC_X); foundCh(CH_ACC_Y); foundCh(CH_ACC_Z);
    foundCh(CH_ROLL); foundCh(CH_PITCH); foundCh(CH_VIB);
  }
}

static void detectMag() {
  // --- QMC5883L (0x0D) ---
  if (i2cuPresent(MOT_QMC_ADDR)) {
    uint8_t id = 0;
    if (i2cuReadReg(MOT_QMC_ADDR, QMC_REG_CHIP_ID, &id, 1) && id == QMC_ID) {
      if (qmcInit()) {
        s_dev.magOk    = true;
        s_dev.magKind  = MAG_QMC;
        s_dev.magAddr  = MOT_QMC_ADDR;
        s_dev.magScale = 100.0f / 12000.0f;   // ±2 G: 12000 LSB/G -> µT
        LOG_I("QMC5883L: wykryty pod 0x%02X (magnetometr 3 osie, 50 Hz)", MOT_QMC_ADDR);
        foundCh(CH_MAG_X); foundCh(CH_MAG_Y); foundCh(CH_MAG_Z); foundCh(CH_HEADING);
      } else {
        LOG_I("QMC5883L: układ pod 0x%02X nie potwierdził konfiguracji - pomijam", MOT_QMC_ADDR);
      }
    }
  }

  // --- HMC5883L (0x1E) ---
  if (i2cuPresent(MOT_HMC_ADDR) && hmcIdOk()) {
    if (s_dev.magOk) {
      LOG_I("HMC5883L: pomijam kolejny magnetometr pod adresem 0x%02X - obsługiwany jest pierwszy",
            MOT_HMC_ADDR);
    } else if (hmcInit()) {
      s_dev.magOk    = true;
      s_dev.magKind  = MAG_HMC;
      s_dev.magAddr  = MOT_HMC_ADDR;
      s_dev.magScale = 100.0f / 1090.0f;      // ±1,3 G: 1090 LSB/G -> µT
      LOG_I("HMC5883L: wykryty pod 0x%02X (magnetometr 3 osie)", MOT_HMC_ADDR);
      foundCh(CH_MAG_X); foundCh(CH_MAG_Y); foundCh(CH_MAG_Z); foundCh(CH_HEADING);
    } else {
      LOG_I("HMC5883L: układ pod 0x%02X nie potwierdził konfiguracji - pomijam", MOT_HMC_ADDR);
    }
  }

  // --- AK8963 (0x0C) - tylko gdy jest MPU i bypass I2C działa ---
  if (!s_dev.mpuOk) return;
  if (!akIdOk()) return;
  if (s_dev.magOk) {
    LOG_I("AK8963: pomijam magnetometr pod adresem 0x%02X - obsługiwany jest pierwszy", MOT_AK_ADDR);
    return;
  }
  if (!akInit()) {
    LOG_I("AK8963: układ pod 0x%02X nie potwierdził konfiguracji - pomijam", MOT_AK_ADDR);
    return;
  }
  s_dev.magOk    = true;
  s_dev.magKind  = MAG_AK;
  s_dev.magAddr  = MOT_AK_ADDR;
  s_dev.magScale = 0.15f;                     // 0,15 µT/LSB (bez korekty ASA z FUSE ROM)
  LOG_I("AK8963: wykryty pod 0x%02X (magnetometr 3 osie w IMU, 0,15 µT/LSB bez korekty ASA)",
        MOT_AK_ADDR);
  foundCh(CH_MAG_X); foundCh(CH_MAG_Y); foundCh(CH_MAG_Z); foundCh(CH_HEADING);
}

static void detectTof() {
  if (!i2cuPresent(MOT_TOF_ADDR)) return;
  // Oba dalmierze siedzą na 0x29 i tylko jeden może być obecny - pierwszy potwierdzony wygrywa
  if (l0xInit()) {
    s_dev.tofOk   = true;
    s_dev.tofKind = TOF_L0X;
    LOG_I("VL53L0X: wykryty pod 0x%02X (dalmierz ToF, model 0x%02X)", MOT_TOF_ADDR, L0X_ID);
    foundCh(CH_TOF_DIST); foundCh(CH_TOF_AMB);
    return;
  }
  if (l1xInit()) {
    s_dev.tofOk   = true;
    s_dev.tofKind = TOF_L1X;
    LOG_I("VL53L1X: wykryty pod 0x%02X (dalmierz ToF, model 0x%02X)", MOT_TOF_ADDR, L1X_ID);
    foundCh(CH_TOF_L1_DIST); foundCh(CH_TOF_AMB);
    return;
  }
  LOG_I("ToF: pod 0x%02X nie ma VL53L0X/VL53L1X (adres zajęty przez inny układ) - pomijam",
        MOT_TOF_ADDR);
}

static void detectMlx() {
  if (!i2cuPresent(MOT_MLX_ADDR)) return;
  uint16_t rawTa = 0, rawTo = 0;
  if (!mlxReadRaw(MLX_REG_TA, rawTa)) return;          // brak poprawnego PEC = cisza
  if (!mlxReadRaw(MLX_REG_TOBJ, rawTo)) return;
  const float ta = mlxRawToC(rawTa);
  const float to = mlxRawToC(rawTo);
  if (ta < MOT_IR_TA_MIN || ta > MOT_IR_TA_MAX) return;
  if (to < MOT_IR_TO_MIN || to > MOT_IR_TO_MAX) return;
  // Powtórny odczyt obiektu musi dać zbliżoną wartość (układ odświeża wynik ~10 Hz)
  uint16_t rawTo2 = 0;
  if (!mlxReadRaw(MLX_REG_TOBJ, rawTo2)) return;
  if (fabsf(mlxRawToC(rawTo2) - to) > 20.0f) return;

  s_dev.mlxEmissEeprom = 1.0f;
  uint16_t rawEe = 0;
  if (mlxReadRaw(MLX_REG_EMISS, rawEe)) {
    float ee = (float)rawEe / 65535.0f;
    if (ee > 0.02f && ee < 1.2f) s_dev.mlxEmissEeprom = ee;
  }
  s_dev.mlxOk = true;
  LOG_I("MLX90614: wykryty pod 0x%02X (termometr IR, To=%.1f °C, Ta=%.1f °C, emisyjność EEPROM %.2f)",
        MOT_MLX_ADDR, to, ta, s_dev.mlxEmissEeprom);
  foundCh(CH_IR_OBJ); foundCh(CH_IR_AMB);
}

// =============================================================
//  Odczyt - magnetometry
// =============================================================
static void readMag() {
  uint8_t b[8];
  float mx = 0.0f, my = 0.0f, mz = 0.0f;
  bool ok = false;

  if (s_dev.magKind == MAG_QMC) {
    // 0x00..0x06: X LSB/MSB, Y LSB/MSB, Z LSB/MSB, status (bit0 = DRDY)
    if (i2cuReadReg(s_dev.magAddr, QMC_REG_DATA, b, 7) && (b[6] & 0x01)) {
      mx = (float)leS16(b + 0);
      my = (float)leS16(b + 2);
      mz = (float)leS16(b + 4);
      ok = true;
    }
  } else if (s_dev.magKind == MAG_HMC) {
    // 0x03..0x09: X MSB/LSB, Z MSB/LSB, Y MSB/LSB (kolejność HMC!), status
    if (i2cuReadReg(s_dev.magAddr, HMC_REG_DATA, b, 7) && (b[6] & 0x01)) {
      mx = (float)beS16(b + 0);
      mz = (float)beS16(b + 2);
      my = (float)beS16(b + 4);
      ok = true;
    }
  } else if (s_dev.magKind == MAG_AK) {
    // 0x02..0x09: ST1, X LE, Y LE, Z LE, ST2 (odczyt ST2 zwalnia dane)
    if (i2cuReadReg(s_dev.magAddr, AK_REG_ST1, b, 8)) {
      if ((b[0] & 0x08) != 0) {
        ok = false;                                    // przepełnienie - pomiar nieważny
      } else if ((b[0] & 0x01) != 0) {
        mx = (float)leS16(b + 1);
        my = (float)leS16(b + 3);
        mz = (float)leS16(b + 5);
        ok = true;
      }
    }
  }

  if (!ok) {
    if (!s_dev.warnMag) { LOG_W("Magnetometr: brak poprawnego odczytu - kanały wyczyszczone"); s_dev.warnMag = true; }
    clearMagChans();
    return;
  }
  s_dev.warnMag = false;

  const float ux = mx * s_dev.magScale;
  const float uy = my * s_dev.magScale;
  const float uz = mz * s_dev.magScale;
  pubGated(CH_MAG_X, ux, MOT_MAG_MIN, MOT_MAG_MAX);
  pubGated(CH_MAG_Y, uy, MOT_MAG_MIN, MOT_MAG_MAX);
  pubGated(CH_MAG_Z, uz, MOT_MAG_MIN, MOT_MAG_MAX);
  pubGated(CH_HEADING, magHeading(ux, uy), 0.0f, 360.0f);
}

// =============================================================
//  Odczyt - IMU i akcelerometry
// =============================================================
static void readMpu() {
  uint8_t b[14];
  if (!i2cuReadReg(s_dev.mpuAddr, MPU_REG_ACC_OUT, b, 14)) {
    if (!s_dev.warnImu) { LOG_W("IMU: brak odczytu z 0x%02X - kanały wyczyszczone", s_dev.mpuAddr); s_dev.warnImu = true; }
    clearImuChans();
    return;
  }
  s_dev.warnImu = false;

  const float ax = (float)beS16(b + 0) * s_dev.accScale;
  const float ay = (float)beS16(b + 2) * s_dev.accScale;
  const float az = (float)beS16(b + 4) * s_dev.accScale;
  const float t  = (float)beS16(b + 6) * s_dev.imuTScale + s_dev.imuTOff;
  const float gx = (float)beS16(b + 8) * s_dev.gyroScale;
  const float gy = (float)beS16(b + 10) * s_dev.gyroScale;
  const float gz = (float)beS16(b + 12) * s_dev.gyroScale;

  float roll = 0.0f, pitch = 0.0f;
  accAngles(ax, ay, az, roll, pitch);

  pubGated(CH_ACC_X, ax, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ACC_Y, ay, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ACC_Z, az, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_GYRO_X, gx, -MOT_GYRO_MAX, MOT_GYRO_MAX);
  pubGated(CH_GYRO_Y, gy, -MOT_GYRO_MAX, MOT_GYRO_MAX);
  pubGated(CH_GYRO_Z, gz, -MOT_GYRO_MAX, MOT_GYRO_MAX);
  pubGated(CH_ROLL, roll, -180.0f, 180.0f);
  pubGated(CH_PITCH, pitch, -90.0f, 90.0f);
  pubGated(CH_VIB, vibeOf(ax, ay, az), 0.0f, MOT_VIB_MAX);
  pubGated(CH_IMU_T, t, MOT_IMU_T_MIN, MOT_IMU_T_MAX);
}

static void readLis() {
  uint8_t b[6];
  // AUTO_INCREMENT (bit7 adresu) - 6 bajtów od 0x28
  if (!i2cuReadReg(s_dev.lisAddr, (uint8_t)(LIS_REG_OUT_X_L | 0x80), b, 6)) {
    if (!s_dev.warnImu) { LOG_W("LIS3DH: brak odczytu z 0x%02X - kanały wyczyszczone", s_dev.lisAddr); s_dev.warnImu = true; }
    clearAccChans();
    return;
  }
  s_dev.warnImu = false;

  const float ax = (float)leS16(b + 0) * s_dev.lisScale;
  const float ay = (float)leS16(b + 2) * s_dev.lisScale;
  const float az = (float)leS16(b + 4) * s_dev.lisScale;
  float roll = 0.0f, pitch = 0.0f;
  accAngles(ax, ay, az, roll, pitch);

  pubGated(CH_ACC_X, ax, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ACC_Y, ay, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ACC_Z, az, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ROLL, roll, -180.0f, 180.0f);
  pubGated(CH_PITCH, pitch, -90.0f, 90.0f);
  pubGated(CH_VIB, vibeOf(ax, ay, az), 0.0f, MOT_VIB_MAX);
}

static void readAdxl() {
  uint8_t b[6];
  if (!i2cuReadReg(s_dev.adxlAddr, ADXL_REG_DATA, b, 6)) {
    if (!s_dev.warnImu) { LOG_W("ADXL345: brak odczytu z 0x%02X - kanały wyczyszczone", s_dev.adxlAddr); s_dev.warnImu = true; }
    clearAccChans();
    return;
  }
  s_dev.warnImu = false;

  const float ax = (float)leS16(b + 0) / ADXL_LSB_PER_G;
  const float ay = (float)leS16(b + 2) / ADXL_LSB_PER_G;
  const float az = (float)leS16(b + 4) / ADXL_LSB_PER_G;
  float roll = 0.0f, pitch = 0.0f;
  accAngles(ax, ay, az, roll, pitch);

  pubGated(CH_ACC_X, ax, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ACC_Y, ay, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ACC_Z, az, -MOT_ACC_MAX, MOT_ACC_MAX);
  pubGated(CH_ROLL, roll, -180.0f, 180.0f);
  pubGated(CH_PITCH, pitch, -90.0f, 90.0f);
  pubGated(CH_VIB, vibeOf(ax, ay, az), 0.0f, MOT_VIB_MAX);
}

// =============================================================
//  Odczyt - dalmierz ToF (maszyna stanów: start -> wynik)
// =============================================================
static void tofFail(const char* who) {
  if (!s_dev.warnTof) {
    LOG_W("%s: brak poprawnego odczytu - kanały wyczyszczone", who);
    s_dev.warnTof = true;
  }
  clearTofChans();
}

static void l0xResult() {
  uint8_t b[12];
  if (!i2cuReadReg(MOT_TOF_ADDR, L0X_REG_RESULT_STATUS, b, sizeof(b))) {
    tofFail("VL53L0X");
    return;
  }
  i2cuWriteReg(MOT_TOF_ADDR, L0X_REG_INT_CLEAR, 0x01);
  const uint8_t rstat = (uint8_t)((b[0] >> 3) & 0x0F);
  const uint16_t range = (uint16_t)(((uint16_t)b[10] << 8) | b[11]);
  const float amb = (float)(((uint16_t)b[4] << 8) | b[5]);   // część całkowita tempa otoczenia [MCPS]
  if (rstat != L0X_STATUS_VALID) {                           // 11 = "Range Valid"
    tofFail("VL53L0X");
    return;
  }
  s_dev.warnTof = false;
  pubGated(CH_TOF_DIST, (float)range, 0.0f, MOT_TOF_MM_MAX);
  pubGated(CH_TOF_AMB, amb, 0.0f, MOT_TOF_AMB_MAX);
  clearCh(CH_TOF_L1_DIST);
}

static void l1xResult() {
  uint8_t b[17];
  if (!l1xReadBlock(L1X_REG_RESULT_STATUS, b, sizeof(b))) {
    tofFail("VL53L1X");
    return;
  }
  l1xWrite8(L1X_REG_INT_CLEAR, 0x01);
  const uint8_t rstat = (uint8_t)((b[0] >> 3) & 0x0F);
  const uint16_t range = (uint16_t)(((uint16_t)b[13] << 8) | b[14]);   // rejestr 0x0096
  const float amb = (float)(((uint16_t)b[3] << 8) | b[4]);             // rejestr 0x008C, górne słowo
  // 0 = "range valid" wg API ST; 11 dopuszczamy dla notacji rejestrowej (jak VL53L0X)
  if (!(rstat == 0 || rstat == L0X_STATUS_VALID)) {
    tofFail("VL53L1X");
    return;
  }
  s_dev.warnTof = false;
  pubGated(CH_TOF_L1_DIST, (float)range, 0.0f, MOT_TOF_MM_MAX);
  pubGated(CH_TOF_AMB, amb, 0.0f, MOT_TOF_AMB_MAX);
  clearCh(CH_TOF_DIST);
}

static void readTof() {
  if (s_dev.tofKind == TOF_L0X) {
    if (s_dev.tofRun) {
      uint8_t st = 0;
      if (i2cuReadReg(MOT_TOF_ADDR, L0X_REG_RESULT_INT_STAT, &st, 1) && (st & 0x07)) {
        s_dev.tofRun = false;
        l0xResult();
        if (l0xStart()) { s_dev.tofRun = true; s_dev.tofMs = millis(); }   // od razu następny pomiar
        return;
      }
      if (millis() - s_dev.tofMs > MOT_TOF_TIMEOUT_MS) {
        s_dev.tofRun = false;
        tofFail("VL53L0X");
      }
      return;                                        // pomiar w toku - nie blokujemy
    }
    if (l0xStart()) { s_dev.tofRun = true; s_dev.tofMs = millis(); }
    else            { tofFail("VL53L0X"); }
    return;
  }

  if (s_dev.tofKind == TOF_L1X) {
    if (s_dev.tofRun) {
      const uint32_t el = millis() - s_dev.tofMs;
      if (el < MOT_TOF_MIN_WAIT_MS) return;           // pomiar jeszcze trwa
      if (el > MOT_TOF_TIMEOUT_MS) {
        s_dev.tofRun = false;
        tofFail("VL53L1X");
        return;
      }
      s_dev.tofRun = false;
      l1xResult();
      if (l1xStart()) { s_dev.tofRun = true; s_dev.tofMs = millis(); }
      return;
    }
    if (l1xStart()) { s_dev.tofRun = true; s_dev.tofMs = millis(); }
    else            { tofFail("VL53L1X"); }
  }
}

// =============================================================
//  Odczyt - termometr IR
// =============================================================
static void readMlx() {
  uint16_t rawTa = 0, rawTo = 0;
  if (!mlxReadRaw(MLX_REG_TA, rawTa) || !mlxReadRaw(MLX_REG_TOBJ, rawTo)) {
    if (!s_dev.warnIr) { LOG_W("MLX90614: brak poprawnego odczytu (magistrala/PEC) - kanały wyczyszczone"); s_dev.warnIr = true; }
    clearCh(CH_IR_OBJ); clearCh(CH_IR_AMB);
    return;
  }
  s_dev.warnIr = false;

  const float tamb = mlxRawToC(rawTa);
  float tobj = mlxRawToC(rawTo);
  tobj = mlxCorrect(tobj, tamb, s_dev.mlxEmissEeprom, s_dev.mlxEmiss);
  pubGated(CH_IR_OBJ, tobj, MOT_IR_TO_MIN, MOT_IR_TO_MAX);
  pubGated(CH_IR_AMB, tamb, MOT_IR_TA_MIN, MOT_IR_TA_MAX);
}

// =============================================================
//  Interfejs modułu
// =============================================================
static void beginImpl(const DrvSink& sink) {
  s_sink = sink;
  memset(&s_dev, 0, sizeof(s_dev));

  // Konfiguracja z configu (tylko odczyt - nic nie zapisujemy do NVS/EEPROM)
  s_dev.decl        = config.extraF("mag_decl", 0.0f);
  s_dev.mlxEmiss    = config.extraF("mlx_emiss", 1.0f);
  s_dev.tofOffsetMm = config.extraF("tof_offset_mm", 0.0f);
  s_dev.accFsIdx    = snapIdx(config.extraF("imu_acc_fs", 4.0f), MOT_ACC_FS, 4);
  s_dev.gyroFsIdx   = snapIdx(config.extraF("imu_gyro_fs", 500.0f), MOT_GYRO_FS, 4);

  // Kolejność ma znaczenie: IMU pierwsze, bo odblokowuje bypass I2C do AK8963
  detectImu();
  detectMag();
  detectTof();
  detectMlx();
}

static void readImpl() {
  if (s_dev.magOk) readMag();
  if (s_dev.mpuOk)      readMpu();
  else if (s_dev.lisOk) readLis();
  else if (s_dev.adxlOk) readAdxl();
  if (s_dev.tofOk) readTof();
  if (s_dev.mlxOk) readMlx();
}

const DrvModule drvMotion = { "Ruch/IMU/Mag/IR", CHANS, NCHANS, beginImpl, readImpl };
