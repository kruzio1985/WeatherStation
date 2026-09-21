/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: PolyForm Noncommercial License 1.0.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_ths.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "i2c_util.h"
#include "syslog.h"

// =============================================================
//  Czujniki temperatury / wilgotności / ciśnienia (I2C, ręcznie).
//
//  Obsługiwane układy (bez bibliotek zewnętrznych - tylko Wire):
//    * SHT3x (SHT30/31/35/85)          0x44/0x45  komenda 0x2400, CRC8
//    * AHT10/20/21/25                   0x38      0xE1/0xBE + 0xAC
//    * HDC1080/HDC2010/HDC2080          0x40/0x41 ID 0xFE = 0x5449 ("TI")
//                                                 HDC10xx: rejestr 0x00, HDC2080: 0x0F
//    * Si7021/HTU21D/SHT21              0x40      komenda 0xE3/0xF5/0xE0
//    * TMP117                           0x48..0x4B ID 0x0F = 0x0117
//    * MCP9808                          0x18..0x1F ID 0x07 = 0x0054
//    * BMP280/BME280                    0x76/0x77 WHO_AM_I 0xD0 = 0x58/0x60
//    * BMP388/BMP390                    0x76/0x77 CHIP_ID 0x00 = 0x50 / 0x60
//    * DPS310/DPS368                    0x76/0x77 PRODUCT_ID 0x0D = 0x1x
//    * MS5611                           0x76/0x77 PROM 0xA0..0xAE + D1/D2
//    * LPS22HB / LPS25HB / LPS28DFW     0x5C/0x5D WHO_AM_I 0x0F
//    * MPL3115A2                        0x60      WHO_AM_I 0x0C = 0xC4
//    * SDP810/SDP800 (ciśnienie różn.)  0x25/0x26 komenda 0x3624
//
//  Adresy wspólne (0x40 oraz 0x76/0x77) rozstrzygamy po ID/WHO_AM_I -
//  obsługiwany jest tylko pierwszy potwierdzony układ, pozostałe
//  zgłaszamy raz w logu jako pominięte.  Kanał publikuje wartość
//  wyłącznie wtedy, gdy dany układ został potwierdzony.
// =============================================================

// ---------- Kanały ----------
enum {
  CH_SHT_T = 0, CH_SHT_H,
  CH_AHT_T, CH_AHT_H,
  CH_HDC_T, CH_HDC_H,
  CH_SI70_T, CH_SI70_H,
  CH_TMP117,
  CH_MCP9808,
  CH_BMP280_T, CH_BMP280_P,
  CH_BMP390_T, CH_BMP390_P,
  CH_DPS_T, CH_DPS_P,
  CH_LPS22_T, CH_LPS22_P,
  CH_LPS25_P,
  CH_LPS28_P,
  CH_MPL_T, CH_MPL_P,
  CH_MS5611_T, CH_MS5611_P,
  CH_SDP_DP
};

static const ChanDef CHANS[] = {
  { "ths_sht_t",    "Temperatura (SHT3x)",          "°C",   1, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_sht_h",    "Wilgotność (SHT3x)",           "%",    1, "out", "humidity",    "%",  "mdi:water-percent" },
  { "ths_aht_t",    "Temperatura (AHT2x)",          "°C",   1, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_aht_h",    "Wilgotność (AHT2x)",           "%",    1, "out", "humidity",    "%",  "mdi:water-percent" },
  { "ths_hdc_t",    "Temperatura (HDC1080)",        "°C",   1, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_hdc_h",    "Wilgotność (HDC1080)",         "%",    1, "out", "humidity",    "%",  "mdi:water-percent" },
  { "ths_si70_t",   "Temperatura (Si7021)",         "°C",   1, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_si70_h",   "Wilgotność (Si7021)",          "%",    1, "out", "humidity",    "%",  "mdi:water-percent" },
  { "ths_tmp117",   "Temperatura (TMP117)",         "°C",   2, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_mcp9808",  "Temperatura (MCP9808)",        "°C",   2, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_bmp280_t", "Temperatura (BMP280)",         "°C",   1, "in",  "temperature", "°C", "mdi:thermometer" },
  { "ths_bmp280_p", "Ciśnienie (BMP280)",           "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_bmp390_t", "Temperatura (BMP388/390)",     "°C",   1, "in",  "temperature", "°C", "mdi:thermometer" },
  { "ths_bmp390_p", "Ciśnienie (BMP388/390)",       "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_dps_t",    "Temperatura (DPS310)",         "°C",   1, "in",  "temperature", "°C", "mdi:thermometer" },
  { "ths_dps_p",    "Ciśnienie (DPS310)",           "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_lps22_t",  "Temperatura (LPS22HB)",        "°C",   1, "in",  "temperature", "°C", "mdi:thermometer" },
  { "ths_lps22_p",  "Ciśnienie (LPS22HB)",          "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_lps25_p",  "Ciśnienie (LPS25HB)",          "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_lps28_p",  "Ciśnienie (LPS28DFW)",         "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_mpl_t",    "Temperatura (MPL3115A2)",      "°C",   1, "in",  "temperature", "°C", "mdi:thermometer" },
  { "ths_mpl_p",    "Ciśnienie (MPL3115A2)",        "hPa",  1, "in",  "pressure",    "hPa", "mdi:gauge" },
  { "ths_ms5611_t", "Temperatura (MS5611)",         "°C",   1, "out", "temperature", "°C", "mdi:thermometer" },
  { "ths_ms5611_p", "Ciśnienie (MS5611)",           "hPa",  1, "out", "pressure",    "hPa", "mdi:gauge" },
  { "ths_sdp_dp",   "Ciśnienie różnicowe (SDP810)", "Pa",   1, "out", "pressure",    "Pa", "mdi:gauge" }
};

static const uint16_t NCHANS = sizeof(CHANS) / sizeof(CHANS[0]);

// ---------- Bramki wartości (publikujemy tylko rozsądne pomiary) ----------
#define THS_T_MIN   (-60.0f)     // °C
#define THS_T_MAX   (130.0f)
#define THS_RH_MIN  (0.0f)       // %
#define THS_RH_MAX  (100.0f)
#define THS_P_MIN   (300.0f)     // hPa
#define THS_P_MAX   (1100.0f)
#define THS_DP_MAX  (10000.0f)   // Pa

// ---------- Adresy i komendy ----------
#define THS_SHT_ADDR1     0x44
#define THS_SHT_ADDR2     0x45
#define THS_SHT_CMD_MEAS  0x2400   // pomiar T+RH, powtarzalność wysoka, bez zegara

#define THS_AHT_ADDR      0x38
#define THS_AHT_REG_STAT  0x71     // rejestr statusu
#define THS_AHT_CMD_INIT1 0xE1     // inicjalizacja (AHT10)
#define THS_AHT_CMD_INIT2 0xBE     // inicjalizacja (AHT20/21/25)
#define THS_AHT_CMD_MEAS  0xAC     // wyzwolenie pomiaru (arg 0x3300)
#define THS_AHT_INIT_ARG  0x0800
#define THS_AHT_MEAS_ARG  0x3300

#define THS_HDC_ADDR1     0x40
#define THS_HDC_ADDR2     0x41
#define THS_HDC_REG_TEMP  0x00     // HDC10xx: pomiar T+RH (rejestr wyzwalający)
#define THS_HDC_REG_CFG   0x02     // HDC10xx: konfiguracja (14 bit, tryb sekwencyjny)
#define THS_HDC_REG_ID1   0xFE     // producent (0x5449 = "TI")
#define THS_HDC_REG_ID2   0xFF     // ID układu
#define THS_HDC2_REG_DATA 0x00     // HDC2010/2080: T (LSB, MSB), RH (LSB, MSB)
#define THS_HDC2_REG_MEAS 0x0F     // HDC2010/2080: MEASUREMENT_CONFIG
#define THS_HDC2_MEAS_TRIG 0x01    // HDC2010/2080: bit startu pomiaru
#define THS_HDC_ID_2XXX   0x2050   // HDC2080 - inna mapa rejestrów niż HDC10xx

#define THS_SI_ADDR       0x40
#define THS_SI_CMD_RH     0xF5     // pomiar wilgotności (bez zegara)
#define THS_SI_CMD_TEMP   0xE0     // odczyt temperatury z ostatniego pomiaru RH
#define THS_SI_CMD_TMEAS  0xE3     // pełny pomiar temperatury (bez zegara)
#define THS_SI_CMD_SIGN   0xFE     // podpis elektroniczny (0x4854 = "HT")
#define THS_SI_SIGN_VAL   0x4854
#define THS_SI_CMD_ID2    0xFC     // ID wersji 2 (komenda 0xC9)
#define THS_SI_CMD_ID1    0xFA     // ID wersji 1 (arg 0x0F)
#define THS_SI_ID_ARG2    0xC9
#define THS_SI_ID_ARG1    0x0F

#define THS_TMP_MAX       0x4B     // 0x48..0x4B
#define THS_TMP_REG_TEMP  0x00     // wynik temperatury
#define THS_TMP_REG_CFG   0x01
#define THS_TMP_REG_ID    0x0F     // ID = 0x0117

#define THS_MCP_MIN       0x18     // 0x18..0x1F
#define THS_MCP_REG_TEMP  0x05     // wynik temperatury
#define THS_MCP_REG_CFG   0x01
#define THS_MCP_REG_MAN   0x06     // ID producenta = 0x0054 (Microchip)
#define THS_MCP_REG_ID    0x07     // ID układu = 0x0054 / 0x0400

#define THS_BMP_REG_ID    0xD0     // WHO_AM_I (BMP280/BME280) = 0x58 / 0x60
#define THS_BMP_REG_RESET 0xE0
#define THS_BMP_REG_CAL   0x88     // 24 bajty kalibracji
#define THS_BMP_REG_CFG   0xF4     // osrs_t/osrs_p/tryb
#define THS_BMP_REG_FILT  0xF5
#define THS_BMP_REG_DATA  0xF7     // 6 bajtów: ciśnienie i temperatura

#define THS_390_REG_CHIP  0x00     // CHIP_ID: 0x50 = BMP388, 0x60 = BMP390
#define THS_390_ID_388    0x50
#define THS_390_ID_390    0x60
#define THS_390_REG_STATUS 0x03
#define THS_390_REG_DATA  0x04     // 6 bajtów: ciśnienie (LSB..MSB), temperatura
#define THS_390_REG_PWR   0x1B     // PWR_CTRL
#define THS_390_REG_OSR   0x1C
#define THS_390_REG_RESET 0x7E
#define THS_390_DRDY_P   0x20
#define THS_390_DRDY_T   0x40

#define THS_DPS_REG_PRS   0x00     // ciśnienie (3 bajty)
#define THS_DPS_REG_TMP   0x03     // temperatura (3 bajty)
#define THS_DPS_REG_PRSCFG 0x06
#define THS_DPS_REG_TMPCFG 0x07
#define THS_DPS_REG_MEAS  0x08     // MEAS_CFG
#define THS_DPS_REG_CFG   0x09
#define THS_DPS_REG_RESET 0x0C
#define THS_DPS_REG_PROD  0x0D     // PRODUCT_ID = 0x1x
#define THS_DPS_REG_COEF  0x10     // 18 bajtów kalibracji
#define THS_DPS_REG_TCSRC 0x28     // źródło współczynników temperatury
#define THS_DPS_SENS_RDY  0x40
#define THS_DPS_COEF_RDY  0x80
#define THS_DPS_TMP_RDY   0x20
#define THS_DPS_PRS_RDY   0x10
#define THS_DPS_REQ_TMP   0x02
#define THS_DPS_REQ_PRS   0x01

#define THS_LPS_ADDR1     0x5C
#define THS_LPS_ADDR2     0x5D
#define THS_LPS_REG_ID    0x0F     // WHO_AM_I: 0xB1/0xBD/0xB4
#define THS_LPS_REG_CTRL1 0x10     // LPS22HB/LPS28DFW (LPS25HB ma 0x20)
#define THS_LPS_REG_CTRL2 0x11     // (LPS25HB ma 0x21)
#define THS_LPS25_REG_CTRL1 0x20
#define THS_LPS25_REG_CTRL2 0x21
#define THS_LPS_REG_STATUS 0x27
#define THS_LPS_REG_PXL   0x28
#define THS_LPS_REG_PL    0x29
#define THS_LPS_REG_PH    0x2A
#define THS_LPS_REG_TL    0x2B
#define THS_LPS_REG_TH    0x2C
#define THS_LPS_CTRL2_ONESHOT 0x01
#define THS_LPS_CTRL2_FSMODE  0x40  // LPS28DFW: 0 = tryb 1 (4096 LSB/hPa)

#define THS_MPL_ADDR      0x60
#define THS_MPL_REG_STATUS 0x00
#define THS_MPL_REG_POUT  0x01     // 3 bajty ciśnienia
#define THS_MPL_REG_TOUT  0x04     // 2 bajty temperatury
#define THS_MPL_REG_ID    0x0C     // WHO_AM_I = 0xC4
#define THS_MPL_REG_PTDCFG 0x13
#define THS_MPL_REG_CTRL1 0x26
#define THS_MPL_STATUS_PDR 0x04
#define THS_MPL_STATUS_TDR 0x02

#define THS_MS_REG_RESET  0x1E
#define THS_MS_CMD_PROM   0xA0     // 8 słów PROM co 2 bajty
#define THS_MS_CMD_D1     0x46     // konwersja ciśnienia (OSR 4096)
#define THS_MS_CMD_D2     0x56     // konwersja temperatury (OSR 4096)
#define THS_MS_CMD_ADC    0x00     // odczyt ADC

#define THS_SDP_ADDR1     0x25
#define THS_SDP_ADDR2     0x26
#define THS_SDP_CMD_STOP  0x3FF9   // zatrzymanie pomiaru ciągłego
#define THS_SDP_CMD_RESET 0x0006   // reset programowy
#define THS_SDP_CMD_ID1   0x367C   // pierwsza połowa identyfikatora
#define THS_SDP_CMD_ID2   0xE102   // druga połowa identyfikatora
#define THS_SDP_CMD_TRIG  0x3624   // pomiar jednorazowy ciśnienia różnicowego

// ---------- Zasilanie / stan modułu ----------
static DrvSink s_sink;             // kopia sinku przekazanego w begin()

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
static void pubT(uint8_t ch, float c) {
  if (isnan(c) || !(c > THS_T_MIN && c < THS_T_MAX)) { clearCh(ch); return; }
  publishCh(ch, c);
}

static void pubH(uint8_t ch, float rh) {
  if (isnan(rh) || !(rh >= THS_RH_MIN && rh <= THS_RH_MAX)) { clearCh(ch); return; }
  publishCh(ch, rh);
}

static void pubP(uint8_t ch, float hpa) {
  if (isnan(hpa) || !(hpa > THS_P_MIN && hpa < THS_P_MAX)) { clearCh(ch); return; }
  publishCh(ch, hpa);
}

static void pubDp(uint8_t ch, float pa) {
  if (isnan(pa) || !(pa > -THS_DP_MAX && pa < THS_DP_MAX)) { clearCh(ch); return; }
  publishCh(ch, pa);
}

// ---------- Dane układów ----------
struct ThsDev {
  // SHT3x
  bool     shtOk;
  uint8_t  shtAddr;
  // AHT1x/2x
  bool     ahtOk;
  uint8_t  ahtAddr;
  uint8_t  ahtPhase;      // 0 = bezczynny, 1 = pomiar w toku
  uint32_t ahtMs;
  // HDC1080/2010/2080
  bool     hdcOk;
  bool     hdc2xxx;       // true = HDC2010/2080 (dane LSB-first, wyzwalanie przez 0x0F)
  uint8_t  hdcAddr;
  uint16_t hdcId;
  // Si7021/HTU21D
  bool     siOk;
  uint8_t  siAddr;
  // TMP117 / MCP9808
  bool     tmpOk;
  uint8_t  tmpAddr;
  bool     mcpOk;
  uint8_t  mcpAddr;
  // BMP280/BME280
  bool     bmp280Ok;
  uint8_t  bmp280Addr;
  bool     bmp280Bme;
  uint8_t  bmp280Id;
  int32_t  bmp280Dig[12];   // [0..2] T1..T3, [3..11] P1..P9
  // BMP388/BMP390
  bool     bmp390Ok;
  uint8_t  bmp390Addr;
  uint16_t b390T1, b390T2, b390P5, b390P6;
  int8_t   b390T3, b390P3, b390P4, b390P7, b390P8, b390P10, b390P11;
  int16_t  b390P1, b390P2, b390P9;
  // DPS310/DPS368
  bool     dpsOk;
  uint8_t  dpsAddr;
  uint8_t  dpsPhase;      // 0 = bezczynny, 1 = czeka na temperaturę, 2 = czeka na ciśnienie
  uint32_t dpsMs;
  int32_t  dpsRawT, dpsRawP;
  int32_t  dpsC0, dpsC1, dpsC00, dpsC10;
  int16_t  dpsC01, dpsC11, dpsC20, dpsC21, dpsC30;
  // LPS22HB / LPS25HB / LPS28DFW
  bool     lpsOk;
  uint8_t  lpsAddr;
  uint8_t  lpsKind;       // 0 = LPS22HB, 1 = LPS25HB, 2 = LPS28DFW
  uint8_t  lpsC1, lpsC2;  // adresy CTRL_REG1 / CTRL_REG2
  // MPL3115A2
  bool     mplOk;
  uint8_t  mplAddr;
  // MS5611
  bool     msOk;
  uint8_t  msAddr;
  uint16_t msProm[6];     // C1..C6 (indeks 0 = C1)
  uint8_t  msPhase;       // 0 = bezczynny, 1 = czeka na D2, 2 = czeka na D1
  uint32_t msMs;
  uint32_t msD1, msD2;
  // SDP810/SDP800
  bool     sdpOk;
  uint8_t  sdpAddr;
  uint8_t  sdpPhase;      // 0 = bezczynny, 1 = czeka na wynik pomiaru
  uint32_t sdpMs;
};

static ThsDev s_dev;       // zerowane przy starcie

// ---------- Pomocniki lokalne ----------
static inline int16_t beS16(const uint8_t* b) {
  return (int16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static inline uint16_t beU16(const uint8_t* b) {
  return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static inline uint16_t leU16(const uint8_t* b) {
  return (uint16_t)(((uint16_t)b[1] << 8) | b[0]);
}

// Zamiana na liczbę ze znakiem o zadanej liczbie bitów
static inline int32_t twosComplement(int32_t val, uint8_t bits) {
  if (val & ((int32_t)1 << (bits - 1))) val -= ((int32_t)1 << bits);
  return val;
}

// 24-bitowa liczba ze znakiem (b[0] = najstarszy bajt)
static inline int32_t twos24(const uint8_t* b) {
  int32_t v = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | (int32_t)b[2];
  return twosComplement(v, 24);
}

// CRC8 poly 0x31, init 0x00 - format Si7021/HTU21D/SHT21
// (uwaga: i2cuCrc8 używa init 0xFF, czyli CRC Sensiriona)
static uint8_t crc8Init0(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// =============================================================
//  SHT3x (SHT30/SHT31/SHT35/SHT85) - 0x44/0x45
// =============================================================
static bool shtProbe(uint8_t addr) {
  uint8_t b[6];
  if (!i2cuWrite16(addr, THS_SHT_CMD_MEAS)) return false;
  delay(16);                                    // pomiar ~15 ms przy wysokiej powtarzalności
  if (!i2cuRead(addr, b, sizeof(b))) return false;
  return i2cuCheckCrc3(b, 2);                   // dwa słowa z CRC8
}

static void shtClear() {
  clearCh(CH_SHT_T);
  clearCh(CH_SHT_H);
}

static void shtRead() {
  uint8_t b[6];
  if (!i2cuWrite16(s_dev.shtAddr, THS_SHT_CMD_MEAS)) { shtClear(); return; }
  delay(16);
  if (!i2cuRead(s_dev.shtAddr, b, sizeof(b)) || !i2cuCheckCrc3(b, 2)) { shtClear(); return; }
  float t  = -45.0f + 175.0f * (float)beU16(b) / 65535.0f;
  float rh = 100.0f * (float)beU16(b + 3) / 65535.0f;
  pubT(CH_SHT_T, t);
  pubH(CH_SHT_H, rh);
}

// =============================================================
//  AHT10/AHT20/AHT21/AHT25 - 0x38.
//  Wynik pomiaru to 6 bajtów bez CRC - pewność daje bit "calibrated"
//  w rejestrze statusu sprawdzony przy wykrywaniu.
// =============================================================
static bool ahtProbe(uint8_t addr) {
  uint8_t st = 0;
  if (!i2cuReadReg(addr, THS_AHT_REG_STAT, &st, 1)) return false;
  if (!(st & 0x08)) return false;               // układ nie zgłasza kalibracji
  // Inicjalizacja: AHT10 używa 0xE1, AHT20/21/25 - 0xBE; próbujemy oba warianty
  if (!i2cuWrite16Arg(addr, THS_AHT_CMD_INIT1, THS_AHT_INIT_ARG)) return false;
  delay(20);
  if (!i2cuReadReg(addr, THS_AHT_REG_STAT, &st, 1)) return false;
  if (!(st & 0x08)) {
    if (!i2cuWrite16Arg(addr, THS_AHT_CMD_INIT2, THS_AHT_INIT_ARG)) return false;
    delay(20);
    if (!i2cuReadReg(addr, THS_AHT_REG_STAT, &st, 1)) return false;
    if (!(st & 0x08)) return false;
  }
  return true;
}

static void ahtClear() {
  clearCh(CH_AHT_T);
  clearCh(CH_AHT_H);
}

// Pomiar trwa ~80 ms, więc rozbijamy go na dwa cykle odczytu (bez blokowania)
static void ahtRead() {
  if (s_dev.ahtPhase == 0) {
    if (!i2cuWrite16Arg(s_dev.ahtAddr, THS_AHT_CMD_MEAS, THS_AHT_MEAS_ARG)) { ahtClear(); return; }
    s_dev.ahtPhase = 1;
    s_dev.ahtMs = millis();
    return;
  }
  uint32_t el = millis() - s_dev.ahtMs;
  if (el < 90) return;                          // pomiar jeszcze trwa - wracamy w kolejnym cyklu
  s_dev.ahtPhase = 0;
  if (el > 1000) { ahtClear(); return; }        // brak odpowiedzi
  uint8_t b[6];
  if (!i2cuRead(s_dev.ahtAddr, b, sizeof(b))) { ahtClear(); return; }
  float rh = ((float)(((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4))) * 100.0f / 1048576.0f;
  float t  = ((float)((((uint32_t)b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5])) * 200.0f / 1048576.0f - 50.0f;
  pubT(CH_AHT_T, t);
  pubH(CH_AHT_H, rh);
}

// =============================================================
//  HDC1080 / HDC2010 / HDC2080 - 0x40/0x41 (rejestr 0x00 = pomiar)
// =============================================================
static bool hdcProbe(uint8_t addr, uint16_t* id) {
  uint8_t b[2];
  if (!i2cuReadReg(addr, THS_HDC_REG_ID1, b, 2)) return false;
  if (beU16(b) != 0x5449) return false;         // "TI"
  if (!i2cuReadReg(addr, THS_HDC_REG_ID2, b, 2)) return false;
  uint16_t did = beU16(b);
  // HDC1000/1008 (0x1000), HDC1010 (0x1010), HDC1050/1080/2010 (0x1050),
  // HDC2080 (0x2050). Układy HDC302x (0x3050/0x4950) mają inną mapę rejestrów
  // i nie są tu obsługiwane.
  if (did != 0x1000 && did != 0x1010 && did != 0x1050 && did != 0x2050) return false;
  *id = did;
  return true;
}

static bool hdcBegin(uint8_t addr, uint16_t id) {
  if (id == THS_HDC_ID_2XXX) {
    // HDC2080: pomiar T+RH (MEAS_CONF = 00), wyzwalanie osobnym zapisem w read()
    return i2cuWriteReg(addr, THS_HDC2_REG_MEAS, 0x00);
  }
  // HDC10xx: 14-bitowy wynik T i RH, pomiar sekwencyjny, grzałka wyłączona
  return i2cuWrite16Arg(addr, THS_HDC_REG_CFG, 0x0000);
}

static void hdcClear() {
  clearCh(CH_HDC_T);
  clearCh(CH_HDC_H);
}

static void hdcRead() {
  uint8_t b[4];
  if (s_dev.hdc2xxx) {
    if (!i2cuWriteReg(s_dev.hdcAddr, THS_HDC2_REG_MEAS, THS_HDC2_MEAS_TRIG)) { hdcClear(); return; }
    delay(14);                                  // 14-bitowy pomiar T+RH
    if (!i2cuRead(s_dev.hdcAddr, b, sizeof(b))) { hdcClear(); return; }
    float t2  = (float)leU16(b) * 165.0f / 65536.0f - 40.0f;
    float rh2 = (float)leU16(b + 2) * 100.0f / 65536.0f;
    pubT(CH_HDC_T, t2);
    pubH(CH_HDC_H, rh2);
    return;
  }
  if (!i2cuWriteReg(s_dev.hdcAddr, THS_HDC_REG_TEMP, 0x00)) { hdcClear(); return; }
  delay(16);                                    // 14-bitowy pomiar T+RH
  if (!i2cuRead(s_dev.hdcAddr, b, sizeof(b))) { hdcClear(); return; }
  float t  = (float)beU16(b) * 165.0f / 65536.0f - 40.0f;
  float rh = (float)beU16(b + 2) * 100.0f / 65536.0f;
  pubT(CH_HDC_T, t);
  pubH(CH_HDC_H, rh);
}

// =============================================================
//  Si7021 / HTU21D / SHT21 - 0x40.
//  CRC8 poly 0x31, ale z inicjalizacją 0x00 (inaczej niż Sensirion).
// =============================================================
static bool siProbe(uint8_t addr) {
  uint8_t b[8];
  // Podpis elektroniczny: komenda 0xFE, 2 bajty, wartość 0x4854 ("HT")
  if (i2cuWrite8(addr, THS_SI_CMD_SIGN) && i2cuRead(addr, b, 2)) {
    if (beU16(b) == THS_SI_SIGN_VAL) return true;
  }
  // ID wersji 2: komenda 0xFC, argument 0xC9, 6 bajtów w 2 blokach CRC
  if (i2cuWrite16Arg(addr, THS_SI_CMD_ID2, THS_SI_ID_ARG2) && i2cuRead(addr, b, 6)) {
    if ((crc8Init0(b, 2) == b[2]) && (crc8Init0(b + 3, 2) == b[5])) return true;
  }
  // ID wersji 1: komenda 0xFA, argument 0x0F, 8 bajtów w 4 blokach CRC
  if (i2cuWrite16Arg(addr, THS_SI_CMD_ID1, THS_SI_ID_ARG1) && i2cuRead(addr, b, 8)) {
    for (uint8_t i = 0; i < 4; i++) {
      if (crc8Init0(b + i * 2, 2) != b[i * 2 + 2]) return false;
    }
    return true;
  }
  return false;
}

static void siRead() {
  uint8_t b[3];
  bool haveTemp = false;
  if (i2cuWrite8(s_dev.siAddr, THS_SI_CMD_RH)) {
    delay(14);                                  // 12-bitowy pomiar wilgotności
    if (i2cuRead(s_dev.siAddr, b, sizeof(b)) && crc8Init0(b, 2) == b[2]) {
      float rh = -6.0f + 125.0f * (float)(((uint16_t)b[0] << 8) | b[1]) / 65536.0f;
      if (rh < 0.0f) rh = 0.0f;
      if (rh > 100.0f) rh = 100.0f;
      pubH(CH_SI70_H, rh);
      // temperatura jest pamiętana z ostatniego pomiaru wilgotności (komenda 0xE0)
      if (i2cuWrite8(s_dev.siAddr, THS_SI_CMD_TEMP) &&
          i2cuRead(s_dev.siAddr, b, sizeof(b)) && crc8Init0(b, 2) == b[2]) {
        haveTemp = true;
      }
    }
  }
  if (!haveTemp) {
    // HTU21D/SHT21 nie obsługują 0xE0 - pełny pomiar temperatury komendą 0xE3
    if (i2cuWrite8(s_dev.siAddr, THS_SI_CMD_TMEAS)) {
      delay(12);                                // 14-bitowy pomiar temperatury
      if (i2cuRead(s_dev.siAddr, b, sizeof(b)) && crc8Init0(b, 2) == b[2]) haveTemp = true;
    }
  }
  if (haveTemp) {
    float t = -46.85f + 175.72f * (float)(((uint16_t)b[0] << 8) | b[1]) / 65536.0f;
    pubT(CH_SI70_T, t);
  } else {
    clearCh(CH_SI70_T);
  }
}

// =============================================================
//  TMP117 - 0x48..0x4B (ID 0x0F = 0x0117)
// =============================================================
static bool tmpProbe(uint8_t* addr) {
  uint8_t b[2];
  for (uint8_t a = 0x48; a <= THS_TMP_MAX; a++) {
    if (!i2cuReadReg(a, THS_TMP_REG_ID, b, 2)) continue;
    uint16_t id = beU16(b);
    if (id == 0x0117 || id == 0x0116) {         // TMP117 / TMP116
      *addr = a;
      return true;
    }
  }
  return false;
}

static void tmpRead() {
  uint8_t b[2];
  if (!i2cuReadReg(s_dev.tmpAddr, THS_TMP_REG_TEMP, b, 2)) { clearCh(CH_TMP117); return; }
  pubT(CH_TMP117, (float)beS16(b) * 0.0078125f);
}

// =============================================================
//  MCP9808 - 0x18..0x1F (ID 0x07 = 0x0054)
// =============================================================
static bool mcpProbe(uint8_t* addr) {
  uint8_t b[2];
  for (uint8_t a = THS_MCP_MIN; a <= 0x1F; a++) {
    if (!i2cuReadReg(a, THS_MCP_REG_MAN, b, 2)) continue;
    if (beU16(b) != 0x0054) continue;           // ID producenta Microchip
    if (!i2cuReadReg(a, THS_MCP_REG_ID, b, 2)) continue;
    uint16_t id = beU16(b);
    if (id == 0x0054 || id == 0x0400) { *addr = a; return true; }
  }
  return false;
}

static void mcpRead() {
  uint8_t b[2];
  if (!i2cuReadReg(s_dev.mcpAddr, THS_MCP_REG_TEMP, b, 2)) { clearCh(CH_MCP9808); return; }
  uint16_t raw = beU16(b);
  int16_t t = (int16_t)(raw & 0x0FFF);
  if (raw & 0x1000) t -= 8192;                  // ujemna temperatura
  pubT(CH_MCP9808, (float)t * 0.0625f);
}

// =============================================================
//  BMP280 / BME280 - 0x76/0x77 (WHO_AM_I 0xD0 = 0x58 / 0x60)
//  Kompensacja wg noty Boscha (BMP280 datasheet, wersja 64-bitowa).
// =============================================================
static bool bmp280Probe(uint8_t addr, uint8_t* id) {
  uint8_t v = 0;
  if (!i2cuReadReg(addr, THS_BMP_REG_ID, &v, 1)) return false;
  if (v != 0x58 && v != 0x60) return false;
  *id = v;
  return true;
}

static bool bmp280Begin(uint8_t addr, uint8_t id) {
  if (!i2cuWriteReg(addr, THS_BMP_REG_RESET, 0xB6)) return false;   // reset - kalibracja z NVM
  delay(3);
  uint8_t c[24];
  if (!i2cuReadReg(addr, THS_BMP_REG_CAL, c, sizeof(c))) return false;
  int32_t* d = s_dev.bmp280Dig;
  d[0]  = (int32_t)(((uint16_t)c[1] << 8) | c[0]);                  // T1 (bez znaku)
  d[1]  = (int32_t)beS16(c + 2);                                    // T2
  d[2]  = (int32_t)beS16(c + 4);                                    // T3
  d[3]  = (int32_t)(((uint16_t)c[7] << 8) | c[6]);                  // P1 (bez znaku)
  for (uint8_t i = 0; i < 8; i++) d[4 + i] = (int32_t)beS16(c + 8 + i * 2);   // P2..P9
  if (d[0] == 0 || d[0] == 0xFFFF) return false;                    // kalibracja nie do użycia
  s_dev.bmp280Bme = (id == 0x60);
  // tryb forced: osrs_t = 1x (001), osrs_p = 4x (010), tryb 01
  if (!i2cuWriteReg(addr, THS_BMP_REG_CFG, 0x45)) return false;
  if (!i2cuWriteReg(addr, THS_BMP_REG_FILT, 0xA0)) return false;    // filtr IIR wyłączony
  return true;
}

static float bmp280CompT(int32_t adcT, int32_t* tFine) {
  const int32_t* d = s_dev.bmp280Dig;
  int32_t v1 = ((((adcT >> 3) - (d[0] << 1))) * d[1]) >> 11;
  int32_t v2 = (((((adcT >> 4) - d[0]) * ((adcT >> 4) - d[0])) >> 12) * d[2]) >> 14;
  *tFine = v1 + v2;
  return (float)((*tFine * 5 + 128) >> 8) / 100.0f;                 // °C
}

static float bmp280CompP(int32_t adcP, int32_t tFine) {
  const int32_t* d = s_dev.bmp280Dig;
  int64_t v1 = ((int64_t)tFine) - 128000;
  int64_t v2 = v1 * v1 * (int64_t)d[8];                             // P6
  v2 = v2 + ((v1 * (int64_t)d[7]) << 17);                           // P5
  v2 = v2 + (((int64_t)d[6]) << 35);                                // P4
  v1 = ((v1 * v1 * (int64_t)d[5]) >> 8) + ((v1 * (int64_t)d[4]) << 12);   // P3, P2
  v1 = (((((int64_t)1) << 47) + v1) * ((int64_t)d[3])) >> 33;       // P1
  if (v1 == 0) return NAN;
  int64_t p = 1048576 - adcP;
  p = (((p << 31) - v2) * 3125) / v1;
  int64_t a = (((int64_t)d[11]) * (p >> 13) * (p >> 13)) >> 25;      // P9
  int64_t b = (((int64_t)d[10]) * p) >> 19;                         // P8
  p = ((p + a + b) >> 8) + (((int64_t)d[9]) << 4);                  // P7
  return (float)p / 25600.0f;                                       // hPa
}

static void bmp280Clear() {
  clearCh(CH_BMP280_T);
  clearCh(CH_BMP280_P);
}

static void bmp280Read() {
  uint8_t b[6];
  if (!i2cuWriteReg(s_dev.bmp280Addr, THS_BMP_REG_CFG, 0x45)) { bmp280Clear(); return; }
  delay(15);                                    // pomiar w trybie forced
  if (!i2cuReadReg(s_dev.bmp280Addr, THS_BMP_REG_DATA, b, sizeof(b))) { bmp280Clear(); return; }
  int32_t adcP = ((int32_t)b[0] << 12) | ((int32_t)b[1] << 4) | (b[2] >> 4);
  int32_t adcT = ((int32_t)b[3] << 12) | ((int32_t)b[4] << 4) | (b[5] >> 4);
  int32_t tFine = 0;
  float t = bmp280CompT(adcT, &tFine);
  float p = bmp280CompP(adcP, tFine);
  pubT(CH_BMP280_T, t);
  pubP(CH_BMP280_P, p);
}

// =============================================================
//  BMP388 / BMP390 - 0x76/0x77 (CHIP_ID 0x00 = 0x50 dla BMP388, 0x60 dla BMP390)
//  Kompensacja wg noty Boscha (BMP388 datasheet).
// =============================================================
static bool bmp390Probe(uint8_t addr, uint8_t* id) {
  uint8_t v = 0;
  if (!i2cuReadReg(addr, THS_390_REG_CHIP, &v, 1)) return false;
  if (v != THS_390_ID_388 && v != THS_390_ID_390) return false;
  // Dodatkowe potwierdzenie: po resecie programowym CHIP_ID musi się powtórzyć
  // (odrzuca układy, które na rejestrze 0x00 zwracają przypadkowy bajt, np. MS5611).
  if (!i2cuWriteReg(addr, THS_390_REG_RESET, 0xB6)) return false;
  delay(3);
  uint8_t v2 = 0;
  if (!i2cuReadReg(addr, THS_390_REG_CHIP, &v2, 1)) return false;
  if (v2 != v) return false;
  *id = v;
  return true;
}

static bool bmp390Begin(uint8_t addr) {
  if (!i2cuWriteReg(addr, THS_390_REG_RESET, 0xB6)) return false;
  delay(3);
  uint8_t c[21];
  if (!i2cuReadReg(addr, 0x31, c, sizeof(c))) return false;
  // kalibracja w kolejności LSB/MSB (tak jak w API Boscha)
  s_dev.b390T1  = (uint16_t)c[1] << 8 | c[0];
  s_dev.b390T2  = (uint16_t)c[3] << 8 | c[2];
  s_dev.b390T3  = (int8_t)c[4];
  s_dev.b390P1  = (int16_t)((uint16_t)c[6] << 8 | c[5]);
  s_dev.b390P2  = (int16_t)((uint16_t)c[8] << 8 | c[7]);
  s_dev.b390P3  = (int8_t)c[9];
  s_dev.b390P4  = (int8_t)c[10];
  s_dev.b390P5  = (uint16_t)c[12] << 8 | c[11];
  s_dev.b390P6  = (uint16_t)c[14] << 8 | c[13];
  s_dev.b390P7  = (int8_t)c[15];
  s_dev.b390P8  = (int8_t)c[16];
  s_dev.b390P9  = (int16_t)((uint16_t)c[18] << 8 | c[17]);
  s_dev.b390P10 = (int8_t)c[19];
  s_dev.b390P11 = (int8_t)c[20];
  if (s_dev.b390T1 == 0 || s_dev.b390T1 == 0xFFFF) return false;
  if (!i2cuWriteReg(addr, THS_390_REG_OSR, 0x11)) return false;     // temperatura 1x, ciśnienie 2x
  if (!i2cuWriteReg(addr, THS_390_REG_PWR, 0x13)) return false;     // pomiar w trybie forced
  return true;
}

static void bmp390Clear() {
  clearCh(CH_BMP390_T);
  clearCh(CH_BMP390_P);
}

static void bmp390Read() {
  if (!i2cuWriteReg(s_dev.bmp390Addr, THS_390_REG_PWR, 0x13)) { bmp390Clear(); return; }
  uint8_t st = 0;
  bool ready = false;
  for (uint8_t i = 0; i < 18; i++) {            // max ~18 ms oczekiwania na wynik
    if (!i2cuReadReg(s_dev.bmp390Addr, THS_390_REG_STATUS, &st, 1)) { bmp390Clear(); return; }
    if ((st & (THS_390_DRDY_P | THS_390_DRDY_T)) == (THS_390_DRDY_P | THS_390_DRDY_T)) { ready = true; break; }
    delay(1);
  }
  if (!ready) { bmp390Clear(); return; }
  uint8_t b[6];
  if (!i2cuReadReg(s_dev.bmp390Addr, THS_390_REG_DATA, b, sizeof(b))) { bmp390Clear(); return; }
  int32_t rawP = ((int32_t)b[2] << 16) | ((int32_t)b[1] << 8) | (int32_t)b[0];
  int32_t rawT = ((int32_t)b[5] << 16) | ((int32_t)b[4] << 8) | (int32_t)b[3];
  // --- temperatura: wynik pośredni t_lin (2^16 °C), potem setne części °C ---
  int64_t pd1 = (int64_t)rawT - (int64_t)256 * (int64_t)s_dev.b390T1;
  int64_t pd2 = (int64_t)s_dev.b390T2 * pd1;
  int64_t pd3 = pd1 * pd1;
  int64_t pd4 = pd3 * (int64_t)s_dev.b390T3;
  int64_t tLin = ((pd2 * 262144) + pd4) / 4294967296;           // t_lin
  float t = (float)((tLin * 25) / 16384) / 100.0f;              // °C
  // --- ciśnienie (offset/wzmocnienie wg noty Boscha) ---
  pd1 = tLin * tLin;
  pd2 = pd1 / 64;
  pd3 = (pd2 * tLin) / 256;
  pd4 = ((int64_t)s_dev.b390P8 * pd3) / 32;
  int64_t pd5 = ((int64_t)s_dev.b390P7 * pd1) * 16;
  int64_t pd6 = ((int64_t)s_dev.b390P6 * tLin) * 4194304;
  int64_t offset = (int64_t)s_dev.b390P5 * 140737488355328LL + pd4 + pd5 + pd6;
  pd2 = ((int64_t)s_dev.b390P4 * pd3) / 32;
  pd4 = ((int64_t)s_dev.b390P3 * pd1) * 4;
  pd5 = ((int64_t)s_dev.b390P2 - 16384) * tLin * 2097152;
  int64_t sens = ((int64_t)s_dev.b390P1 - 16384) * 70368744177664LL + pd2 + pd4 + pd5;
  pd1 = (sens / 16777216) * (int64_t)rawP;
  pd2 = (int64_t)s_dev.b390P10 * tLin;
  pd3 = pd2 + (int64_t)65536 * (int64_t)s_dev.b390P9;
  pd4 = (pd3 * (int64_t)rawP) / 8192;
  pd5 = ((int64_t)rawP * (pd4 / 10)) / 512;
  pd5 = pd5 * 10;
  pd6 = (int64_t)rawP * (int64_t)rawP;
  pd2 = ((int64_t)s_dev.b390P11 * pd6) / 65536;
  pd3 = (pd2 * (int64_t)rawP) / 128;
  pd4 = (offset / 4) + pd1 + pd5 + pd3;
  // wynik w setnych częściach paskala -> hPa (/(100*10000))
  float p = (float)((double)pd4 * 25.0 / 1099511627776.0 / 10000.0);
  pubT(CH_BMP390_T, t);
  pubP(CH_BMP390_P, p);
}

// =============================================================
//  DPS310 / DPS368 - 0x76/0x77 (PRODUCT_ID 0x0D = 0x10)
//  Tryb jednorazowy, rozłożony na cykle (temperatura, potem ciśnienie).
// =============================================================
static bool dpsProbe(uint8_t addr) {
  uint8_t v = 0;
  if (!i2cuReadReg(addr, THS_DPS_REG_PROD, &v, 1)) return false;
  return ((v & 0xF0) == 0x10);                  // DPS310 = 0x10, DPS368 = 0x11
}

static bool dpsBegin(uint8_t addr) {
  // Reset + potwierdzenie gotowości współczynników kalibracyjnych
  if (!i2cuWriteReg(addr, THS_DPS_REG_RESET, 0x89)) return false;
  delay(10);
  uint8_t m = 0;
  bool ready = false;
  for (uint8_t i = 0; i < 10; i++) {            // max ~20 ms
    if (!i2cuReadReg(addr, THS_DPS_REG_MEAS, &m, 1)) return false;
    if ((m & (THS_DPS_COEF_RDY | THS_DPS_SENS_RDY)) == (THS_DPS_COEF_RDY | THS_DPS_SENS_RDY)) { ready = true; break; }
    delay(2);
  }
  if (!ready) return false;
  uint8_t c[18];
  if (!i2cuReadReg(addr, THS_DPS_REG_COEF, c, sizeof(c))) return false;
  s_dev.dpsC0  = twosComplement(((int32_t)c[0] << 4) | ((c[1] >> 4) & 0x0F), 12);
  s_dev.dpsC1  = twosComplement(((int32_t)(c[1] & 0x0F) << 8) | c[2], 12);
  s_dev.dpsC00 = twosComplement(((int32_t)c[3] << 12) | ((int32_t)c[4] << 4) | ((c[5] >> 4) & 0x0F), 20);
  s_dev.dpsC10 = twosComplement(((int32_t)(c[5] & 0x0F) << 16) | ((int32_t)c[6] << 8) | c[7], 20);
  s_dev.dpsC01 = (int16_t)(((uint16_t)c[8] << 8) | c[9]);
  s_dev.dpsC11 = (int16_t)(((uint16_t)c[10] << 8) | c[11]);
  s_dev.dpsC20 = (int16_t)(((uint16_t)c[12] << 8) | c[13]);
  s_dev.dpsC21 = (int16_t)(((uint16_t)c[14] << 8) | c[15]);
  s_dev.dpsC30 = (int16_t)(((uint16_t)c[16] << 8) | c[17]);
  uint8_t src = 0;
  if (!i2cuReadReg(addr, THS_DPS_REG_TCSRC, &src, 1)) src = 0;
  // Skala 2^19*3 odpowiada 2-krotnemu nadpróbkowaniu w obu torach
  if (!i2cuWriteReg(addr, THS_DPS_REG_PRSCFG, 0x01)) return false;
  if (!i2cuWriteReg(addr, THS_DPS_REG_TMPCFG, (uint8_t)(0x01 | (src & 0x80)))) return false;
  if (!i2cuWriteReg(addr, THS_DPS_REG_CFG, 0x00)) return false;
  return true;
}

static void dpsClear() {
  clearCh(CH_DPS_T);
  clearCh(CH_DPS_P);
}

static void dpsRead() {
  uint8_t m = 0;
  if (!i2cuReadReg(s_dev.dpsAddr, THS_DPS_REG_MEAS, &m, 1)) { dpsClear(); s_dev.dpsPhase = 0; return; }
  if (s_dev.dpsPhase == 0) {                    // wyzwolenie pomiaru temperatury
    if (!i2cuWriteReg(s_dev.dpsAddr, THS_DPS_REG_MEAS, THS_DPS_REQ_TMP)) { dpsClear(); return; }
    s_dev.dpsPhase = 1;
    s_dev.dpsMs = millis();
    return;
  }
  if (millis() - s_dev.dpsMs > 500) {           // przekroczony czas - restart sekwencji
    dpsClear();
    s_dev.dpsPhase = 0;
    return;
  }
  if (s_dev.dpsPhase == 1) {
    if (!(m & THS_DPS_TMP_RDY)) return;         // wynik jeszcze nie gotowy
    uint8_t b[3];
    if (!i2cuReadReg(s_dev.dpsAddr, THS_DPS_REG_TMP, b, sizeof(b))) { dpsClear(); s_dev.dpsPhase = 0; return; }
    s_dev.dpsRawT = twos24(b);
    if (!i2cuWriteReg(s_dev.dpsAddr, THS_DPS_REG_MEAS, THS_DPS_REQ_PRS)) { dpsClear(); s_dev.dpsPhase = 0; return; }
    s_dev.dpsPhase = 2;
    s_dev.dpsMs = millis();
    return;
  }
  if (!(m & THS_DPS_PRS_RDY)) return;           // wynik ciśnienia jeszcze nie gotowy
  uint8_t b[3];
  if (!i2cuReadReg(s_dev.dpsAddr, THS_DPS_REG_PRS, b, sizeof(b))) { dpsClear(); s_dev.dpsPhase = 0; return; }
  s_dev.dpsRawP = twos24(b);
  s_dev.dpsPhase = 0;
  float tsc = (float)s_dev.dpsRawT / 1572864.0f;
  float psc = (float)s_dev.dpsRawP / 1572864.0f;
  float t = tsc * (float)s_dev.dpsC1 + (float)s_dev.dpsC0 / 2.0f;
  float p = ((float)s_dev.dpsC00 +
             psc * ((float)s_dev.dpsC10 + psc * ((float)s_dev.dpsC20 + psc * (float)s_dev.dpsC30)) +
             tsc * (float)s_dev.dpsC01 +
             tsc * psc * ((float)s_dev.dpsC11 + psc * (float)s_dev.dpsC21)) / 100.0f;   // hPa
  pubT(CH_DPS_T, t);
  pubP(CH_DPS_P, p);
}

// =============================================================
//  LPS22HB / LPS25HB / LPS28DFW - 0x5C/0x5D
// =============================================================
static bool lpsProbe(uint8_t addr, uint8_t* kind, uint8_t* id) {
  uint8_t v = 0;
  if (!i2cuReadReg(addr, THS_LPS_REG_ID, &v, 1)) return false;
  if (v == 0xB1) *kind = 0;                     // LPS22HB
  else if (v == 0xBD) *kind = 1;                // LPS25HB
  else if (v == 0xB4) *kind = 2;                // LPS28DFW
  else return false;
  *id = v;
  return true;
}

static bool lpsBegin(uint8_t addr, uint8_t kind) {
  s_dev.lpsC1 = (kind == 1) ? THS_LPS25_REG_CTRL1 : THS_LPS_REG_CTRL1;
  s_dev.lpsC2 = (kind == 1) ? THS_LPS25_REG_CTRL2 : THS_LPS_REG_CTRL2;
  if (!i2cuWriteReg(addr, s_dev.lpsC1, 0x00)) return false;         // tryb power-down
  uint8_t c2 = 0;
  if (!i2cuReadReg(addr, s_dev.lpsC2, &c2, 1)) c2 = 0;
  if (kind == 2) c2 &= (uint8_t)~THS_LPS_CTRL2_FSMODE;              // LPS28DFW: tryb 1 = 4096 LSB/hPa
  if (!i2cuWriteReg(addr, s_dev.lpsC2, c2)) return false;
  return true;
}

static void lpsClear() {
  clearCh(CH_LPS22_T);
  clearCh(CH_LPS22_P);
  clearCh(CH_LPS25_P);
  clearCh(CH_LPS28_P);
}

static void lpsRead() {
  uint8_t c2 = 0;
  if (!i2cuReadReg(s_dev.lpsAddr, s_dev.lpsC2, &c2, 1)) { lpsClear(); return; }
  if (!i2cuWriteReg(s_dev.lpsAddr, s_dev.lpsC2, (uint8_t)(c2 | THS_LPS_CTRL2_ONESHOT))) { lpsClear(); return; }
  uint8_t st = 0;
  bool ready = false;
  for (uint8_t i = 0; i < 18; i++) {            // max ~18 ms oczekiwania na wynik
    if (!i2cuReadReg(s_dev.lpsAddr, THS_LPS_REG_STATUS, &st, 1)) { lpsClear(); return; }
    if (st & 0x01) { ready = true; break; }
    delay(1);
  }
  if (!ready) { lpsClear(); return; }
  uint8_t px = 0, pl = 0, ph = 0;
  if (!i2cuReadReg(s_dev.lpsAddr, THS_LPS_REG_PXL, &px, 1)) { lpsClear(); return; }
  if (!i2cuReadReg(s_dev.lpsAddr, THS_LPS_REG_PL, &pl, 1)) { lpsClear(); return; }
  if (!i2cuReadReg(s_dev.lpsAddr, THS_LPS_REG_PH, &ph, 1)) { lpsClear(); return; }
  int32_t rawP = ((int32_t)ph << 16) | ((int32_t)pl << 8) | (int32_t)px;
  rawP = twosComplement(rawP, 24);
  float p = (float)rawP / 4096.0f;              // 4096 LSB/hPa
  if (s_dev.lpsKind == 0) {                     // LPS22HB ma kanał temperatury
    uint8_t tl = 0, th = 0;
    if (!i2cuReadReg(s_dev.lpsAddr, THS_LPS_REG_TL, &tl, 1)) { lpsClear(); return; }
    if (!i2cuReadReg(s_dev.lpsAddr, THS_LPS_REG_TH, &th, 1)) { lpsClear(); return; }
    int16_t traw = (int16_t)(((uint16_t)th << 8) | tl);      // 100 LSB/°C, U2
    pubT(CH_LPS22_T, (float)traw / 100.0f);
    pubP(CH_LPS22_P, p);
    return;
  }
  if (s_dev.lpsKind == 1) pubP(CH_LPS25_P, p);
  else pubP(CH_LPS28_P, p);
}

// =============================================================
//  MPL3115A2 - 0x60 (WHO_AM_I 0x0C = 0xC4)
// =============================================================
static bool mplProbe(uint8_t addr) {
  uint8_t v = 0;
  if (!i2cuReadReg(addr, THS_MPL_REG_ID, &v, 1)) return false;
  return (v == 0xC4);
}

static bool mplBegin(uint8_t addr) {
  if (!i2cuWriteReg(addr, THS_MPL_REG_CTRL1, 0x00)) return false;   // tryb czuwania
  if (!i2cuWriteReg(addr, THS_MPL_REG_PTDCFG, 0x07)) return false;  // gotowość danych T i P
  if (!i2cuWriteReg(addr, THS_MPL_REG_CTRL1, 0x08)) return false;   // nadpróbkowanie 1x, czuwanie
  return true;
}

static void mplClear() {
  clearCh(CH_MPL_T);
  clearCh(CH_MPL_P);
}

static void mplRead() {
  if (!i2cuWriteReg(s_dev.mplAddr, THS_MPL_REG_CTRL1, 0x0A)) { mplClear(); return; }   // OST = pomiar jednorazowy
  uint8_t st = 0;
  bool ready = false;
  for (uint8_t i = 0; i < 18; i++) {            // max ~18 ms oczekiwania na wynik
    if (!i2cuReadReg(s_dev.mplAddr, THS_MPL_REG_STATUS, &st, 1)) { mplClear(); return; }
    if ((st & (THS_MPL_STATUS_PDR | THS_MPL_STATUS_TDR)) == (THS_MPL_STATUS_PDR | THS_MPL_STATUS_TDR)) { ready = true; break; }
    delay(1);
  }
  if (!ready) { mplClear(); return; }
  uint8_t p[3];
  if (!i2cuReadReg(s_dev.mplAddr, THS_MPL_REG_POUT, p, sizeof(p))) { mplClear(); return; }
  uint8_t t[2];
  if (!i2cuReadReg(s_dev.mplAddr, THS_MPL_REG_TOUT, t, sizeof(t))) { mplClear(); return; }
  uint32_t rawP = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
  float hpa = ((float)(rawP >> 6) + (float)((p[2] >> 4) & 0x03) / 4.0f) / 100.0f;   // wynik w Pa
  pubT(CH_MPL_T, (float)beS16(t) / 16.0f);
  pubP(CH_MPL_P, hpa);
}

// =============================================================
//  MS5611 - 0x76/0x77 (PROM 0xA0..0xAE, CRC4)
//  Kompensacja wg noty: wynik ciśnienia jest w setnych hPa.
// =============================================================
// CRC4 z noty MS5611 (rejestr 16-bit, wielomian 0x3000) - wersja sprawdzona sprzętowo
static uint16_t msCrc4(const uint16_t* w) {
  uint16_t rem = 0;
  for (uint8_t cnt = 0; cnt < 16; cnt++) {
    if (cnt & 1) rem ^= (uint16_t)(w[cnt >> 1] & 0x00FF);
    else         rem ^= (uint16_t)(w[cnt >> 1] >> 8);
    for (uint8_t b = 0; b < 8; b++) {
      if (rem & 0x8000) rem = (uint16_t)((rem << 1) ^ 0x3000);
      else              rem = (uint16_t)(rem << 1);
    }
  }
  return (uint16_t)((rem >> 12) & 0x0F);
}

static bool msCrcOk(const uint16_t* prom) {
  uint16_t w[8];
  uint16_t crc, calc;
  // wariant podstawowy: CRC w dolnych 4 bitach słowa 7
  for (uint8_t i = 0; i < 8; i++) w[i] = prom[i];
  crc = (uint16_t)(w[7] & 0x0F);
  w[7] = (uint16_t)(w[7] & 0xFF00);
  calc = msCrc4(w);
  if (calc == crc) return true;
  // to samo, ale z wyzerowanymi górnymi 4 bitami słowa 0 (tak jak w nocie)
  w[0] = (uint16_t)(prom[0] & 0x0FFF);
  calc = msCrc4(w);
  if (calc == crc) return true;
  // wariant alternatywny (MS5637): CRC w górnych 4 bitach słowa 0
  for (uint8_t i = 0; i < 8; i++) w[i] = prom[i];
  crc = (uint16_t)((w[0] & 0xF000) >> 12);
  w[0] = (uint16_t)(w[0] & 0x0FFF);
  w[7] = 0;
  return (msCrc4(w) == crc);
}

// Kontrola sensowności współczynników C1..C6 - zabezpieczenie, gdy CRC nie pasuje
static bool msPromPlaus(const uint16_t* prom) {
  for (uint8_t i = 1; i <= 6; i++) {
    if (prom[i] == 0x0000 || prom[i] == 0xFFFF) return false;
  }
  for (uint8_t i = 1; i <= 2; i++) {            // C1 (czułość) i C2 (offset)
    if (prom[i] < 1000 || prom[i] > 65000) return false;
  }
  for (uint8_t i = 5; i <= 6; i++) {            // C5 (Tref) i C6 (TEMPSENS)
    if (prom[i] < 1000 || prom[i] > 65000) return false;
  }
  return (prom[1] != 0 && prom[0] != 0xFFFF);
}

static bool msProbe(uint8_t addr) {
  if (!i2cuWrite8(addr, THS_MS_REG_RESET)) return false;
  delay(3);
  uint16_t prom[8];                             // 0 = słowo fabryczne, 1..6 = C1..C6, 7 = CRC
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b[2];
    if (!i2cuWrite8(addr, (uint8_t)(THS_MS_CMD_PROM + i * 2))) return false;
    if (!i2cuRead(addr, b, sizeof(b))) return false;
    prom[i] = beU16(b);
  }
  if (prom[0] == 0xFFFF) return false;
  if (!msCrcOk(prom) && !msPromPlaus(prom)) return false;   // CRC albo wiarygodne współczynniki
  for (uint8_t i = 0; i < 6; i++) s_dev.msProm[i] = prom[i + 1];
  return true;
}

static void msClear() {
  clearCh(CH_MS5611_T);
  clearCh(CH_MS5611_P);
}

static void msRead() {
  if (s_dev.msPhase == 0) {                     // konwersja temperatury (D2)
    if (!i2cuWrite8(s_dev.msAddr, THS_MS_CMD_D2)) { msClear(); return; }
    s_dev.msPhase = 1;
    s_dev.msMs = millis();
    return;
  }
  uint32_t el = millis() - s_dev.msMs;
  if (el < 10) return;                          // konwersja 4096 trwa ~9 ms - nie blokujemy
  uint8_t b[3];
  if (s_dev.msPhase == 1) {
    if (!i2cuWrite8(s_dev.msAddr, THS_MS_CMD_ADC) || !i2cuRead(s_dev.msAddr, b, sizeof(b))) { msClear(); s_dev.msPhase = 0; return; }
    s_dev.msD2 = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    if (!i2cuWrite8(s_dev.msAddr, THS_MS_CMD_D1)) { msClear(); s_dev.msPhase = 0; return; }
    s_dev.msPhase = 2;
    s_dev.msMs = millis();
    return;
  }
  if (el > 200) { msClear(); s_dev.msPhase = 0; return; }
  if (!i2cuWrite8(s_dev.msAddr, THS_MS_CMD_ADC) || !i2cuRead(s_dev.msAddr, b, sizeof(b))) { msClear(); s_dev.msPhase = 0; return; }
  s_dev.msD1 = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
  s_dev.msPhase = 0;
  const uint16_t* c = s_dev.msProm;             // C1..C6
  int64_t dT = (int64_t)s_dev.msD2 - ((int64_t)c[4] << 8);
  int64_t temp = 2000 + (dT * (int64_t)c[5]) / 8388608;                 // setne °C
  int64_t off = ((int64_t)c[1] << 16) + (dT * (int64_t)c[3]) / 128;     // /2^7
  int64_t sens = ((int64_t)c[0] << 15) + (dT * (int64_t)c[2]) / 256;    // /2^8
  int64_t t2 = 0, off2 = 0, sens2 = 0;
  if (temp < 2000) {                            // korekcja drugiego rzędu
    t2 = (dT * dT) / 2147483648;                // /2^31
    off2 = 5 * (temp - 2000) * (temp - 2000) / 2;
    sens2 = 5 * (temp - 2000) * (temp - 2000) / 4;
    if (temp < -1500) {
      off2 += 7 * (temp + 1500) * (temp + 1500);
      sens2 += 11 * (temp + 1500) * (temp + 1500) / 2;
    }
  }
  temp -= t2;
  off -= off2;
  sens -= sens2;
  int64_t p = (((int64_t)s_dev.msD1 * sens) / 2097152 - off) / 32768;   // setne hPa
  pubT(CH_MS5611_T, (float)temp / 100.0f);
  pubP(CH_MS5611_P, (float)p / 100.0f);
}

// =============================================================
//  SDP810 / SDP800 - 0x25/0x26 (ciśnienie różnicowe, komenda 0x3624)
//  Pomiar jednorazowy trwa ~45 ms, więc wynik odbieramy w kolejnym
//  cyklu odczytu - bez blokowania pętli.
// =============================================================
// Identyfikator: dwie komendy 16-bit podane bez przerwy, potem 6 słów (18 B) z CRC.
// Słowo nr 2 zawiera rodzinę układu (0x02 = SDP8xx, 0x01 = SDP3x).
static bool sdpReadId(uint8_t addr, uint8_t* family) {
  uint8_t b[18];
  if (!i2cuWrite16(addr, THS_SDP_CMD_ID1)) return false;
  if (!i2cuWrite16(addr, THS_SDP_CMD_ID2)) return false;
  delay(2);
  if (!i2cuRead(addr, b, sizeof(b)) || !i2cuCheckCrc3(b, 6)) return false;
  if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF) return false;   // magistrala "wisi" w stanie wysokim
  *family = (uint8_t)(beU16(b + 3) >> 8);
  return true;
}

static bool sdpProbe(uint8_t addr) {
  uint8_t family = 0;
  if (!i2cuWrite16(addr, THS_SDP_CMD_STOP)) return false;           // koniec ewentualnego pomiaru ciągłego
  if (!i2cuWrite16(addr, THS_SDP_CMD_RESET)) return false;
  delay(20);                                                        // reset trwa ~20 ms
  if (!sdpReadId(addr, &family)) return false;
  if (family != 0x02) return false;                                 // obcy układ (np. SDP3x - rodzina 0x01)
  // pomiar potwierdzający format wyniku (9 bajtów w 3 blokach CRC)
  if (!i2cuWrite16(addr, THS_SDP_CMD_TRIG)) return false;
  delay(45);
  uint8_t b[9];
  if (!i2cuRead(addr, b, sizeof(b)) || !i2cuCheckCrc3(b, 3)) return false;
  int16_t scale = (int16_t)(((uint16_t)b[6] << 8) | b[7]);
  return (scale > 0 && scale < 1000);                               // 60 (500 Pa) lub 240 (125 Pa)
}

static void sdpRead() {
  uint8_t b[9];
  if (s_dev.sdpPhase == 0) {
    if (!i2cuWrite16(s_dev.sdpAddr, THS_SDP_CMD_TRIG)) { clearCh(CH_SDP_DP); return; }
    s_dev.sdpPhase = 1;
    s_dev.sdpMs = millis();
    return;
  }
  uint32_t el = millis() - s_dev.sdpMs;
  if (el < 50) return;                          // pomiar trwa ~45 ms - wracamy w kolejnym cyklu
  s_dev.sdpPhase = 0;
  if (el > 1000) { clearCh(CH_SDP_DP); return; }
  if (!i2cuRead(s_dev.sdpAddr, b, sizeof(b)) || !i2cuCheckCrc3(b, 3)) { clearCh(CH_SDP_DP); return; }
  int16_t raw = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
  int16_t scale = (int16_t)(((uint16_t)b[6] << 8) | b[7]);
  if (scale <= 0 || scale >= 1000) { clearCh(CH_SDP_DP); return; }
  pubDp(CH_SDP_DP, (float)raw / (float)scale);  // wynik w Pa
}

// =============================================================
//  Start - wykrywanie układów
// =============================================================
static void beginImpl(const DrvSink& s) {
  s_sink = s;
  memset(&s_dev, 0, sizeof(s_dev));

  // --- SHT3x: 0x44, potem 0x45 ---
  if (shtProbe(THS_SHT_ADDR1)) { s_dev.shtAddr = THS_SHT_ADDR1; s_dev.shtOk = true; }
  else if (shtProbe(THS_SHT_ADDR2)) { s_dev.shtAddr = THS_SHT_ADDR2; s_dev.shtOk = true; }
  if (s_dev.shtOk) {
    LOG_I("SHT3x: wykryty pod adresem 0x%02X (temperatura i wilgotność)", s_dev.shtAddr);
    foundCh(CH_SHT_T);
    foundCh(CH_SHT_H);
  }

  // --- AHT10/20/21/25: 0x38 ---
  if (ahtProbe(THS_AHT_ADDR)) {
    s_dev.ahtAddr = THS_AHT_ADDR;
    s_dev.ahtOk = true;
    LOG_I("AHT1x/2x: wykryty pod adresem 0x%02X (temperatura i wilgotność)", s_dev.ahtAddr);
    foundCh(CH_AHT_T);
    foundCh(CH_AHT_H);
  }

  // --- 0x40/0x41: najpierw HDC1080/2010/2080, potem Si7021/HTU21D ---
  // Oba układy mogą siedzieć na 0x40, dlatego Si7021 szukamy tylko na adresie,
  // którego nie zajął potwierdzony HDC.
  uint16_t hdcId = 0;
  bool hdcFound = hdcProbe(THS_HDC_ADDR1, &hdcId);
  uint8_t hdcAddr = THS_HDC_ADDR1;
  if (!hdcFound && hdcProbe(THS_HDC_ADDR2, &hdcId)) { hdcFound = true; hdcAddr = THS_HDC_ADDR2; }
  bool siFound = false;
  uint8_t siAddr = THS_SI_ADDR;
  bool siSkipped = (hdcFound && hdcAddr == THS_SI_ADDR);
  if (!siSkipped) {
    if (siProbe(THS_SI_ADDR)) { siFound = true; siSkipped = false; }
  }
  if (!siFound && (!hdcFound || hdcAddr != THS_HDC_ADDR2)) {
    if (siProbe(THS_HDC_ADDR2)) { siFound = true; siAddr = THS_HDC_ADDR2; siSkipped = false; }
  }
  if (hdcFound) {
    if (hdcBegin(hdcAddr, hdcId)) {
      s_dev.hdcAddr = hdcAddr;
      s_dev.hdcId = hdcId;
      s_dev.hdc2xxx = (hdcId == THS_HDC_ID_2XXX);
      s_dev.hdcOk = true;
      LOG_I("HDC1080/2xxx: wykryty pod adresem 0x%02X, ID 0x%04X (temperatura i wilgotność)", hdcAddr, hdcId);
      foundCh(CH_HDC_T);
      foundCh(CH_HDC_H);
    } else {
      LOG_W("HDC10xx/2xxx: układ pod adresem 0x%02X nie odpowiedział na konfigurację", hdcAddr);
    }
    if (siSkipped && !siFound) {
      LOG_I("Si7021/HTU21D: pominięty (adres 0x%02X zajęty przez HDC)", hdcAddr);
    }
  }
  if (siFound) {
    s_dev.siAddr = siAddr;
    s_dev.siOk = true;
    LOG_I("Si7021/HTU21D: wykryty pod adresem 0x%02X (temperatura i wilgotność)", siAddr);
    foundCh(CH_SI70_T);
    foundCh(CH_SI70_H);
  }

  // --- TMP117 ---
  uint8_t tmpAddr = 0;
  if (tmpProbe(&tmpAddr)) {
    if (i2cuWrite16Arg(tmpAddr, THS_TMP_REG_CFG, 0x0220)) {          // konwersja ciągła (wartość domyślna)
      s_dev.tmpAddr = tmpAddr;
      s_dev.tmpOk = true;
      LOG_I("TMP117: wykryty pod adresem 0x%02X (temperatura)", tmpAddr);
      foundCh(CH_TMP117);
    } else {
      LOG_W("TMP117: układ pod adresem 0x%02X nie odpowiedział na konfigurację", tmpAddr);
    }
  }

  // --- MCP9808 ---
  uint8_t mcpAddr = 0;
  if (mcpProbe(&mcpAddr)) {
    if (i2cuWriteReg(mcpAddr, THS_MCP_REG_CFG, 0x00)) {            // konwersja ciągła
      s_dev.mcpAddr = mcpAddr;
      s_dev.mcpOk = true;
      LOG_I("MCP9808: wykryty pod adresem 0x%02X (temperatura)", mcpAddr);
      foundCh(CH_MCP9808);
    } else {
      LOG_W("MCP9808: układ pod adresem 0x%02X nie odpowiedział na konfigurację", mcpAddr);
    }
  }

  // --- 0x76/0x77: BMP280/BME280 -> BMP388/390 -> DPS310 -> MS5611 ---
  uint8_t bmp280Addr = 0, bmp280Id = 0;
  if (bmp280Probe(0x76, &bmp280Id)) bmp280Addr = 0x76;
  else if (bmp280Probe(0x77, &bmp280Id)) bmp280Addr = 0x77;
  if (bmp280Addr) {
    if (bmp280Begin(bmp280Addr, bmp280Id)) {
      s_dev.bmp280Addr = bmp280Addr;
      s_dev.bmp280Ok = true;
      LOG_I("%s: wykryty pod adresem 0x%02X (temperatura i ciśnienie)",
            s_dev.bmp280Bme ? "BME280" : "BMP280", bmp280Addr);
      foundCh(CH_BMP280_T);
      foundCh(CH_BMP280_P);
    } else {
      LOG_W("BMP280/BME280: układ pod adresem 0x%02X nie odpowiedział na kalibrację", bmp280Addr);
      bmp280Addr = 0;
    }
  }

  uint8_t bmp390Addr = 0, bmp390Id = 0;
  if (!bmp280Addr) {
    if (bmp390Probe(0x76, &bmp390Id)) bmp390Addr = 0x76;
    else if (bmp390Probe(0x77, &bmp390Id)) bmp390Addr = 0x77;
    if (bmp390Addr) {
      if (bmp390Begin(bmp390Addr)) {
        s_dev.bmp390Addr = bmp390Addr;
        s_dev.bmp390Ok = true;
        LOG_I("%s: wykryty pod adresem 0x%02X, CHIP_ID 0x%02X (temperatura i ciśnienie)",
              (bmp390Id == THS_390_ID_390) ? "BMP390" : "BMP388", bmp390Addr, bmp390Id);
        foundCh(CH_BMP390_T);
        foundCh(CH_BMP390_P);
      } else {
        LOG_W("BMP388/390: układ pod adresem 0x%02X nie odpowiedział na kalibrację", bmp390Addr);
        bmp390Addr = 0;
      }
    }
  }

  uint8_t dpsAddr = 0;
  if (!bmp280Addr && !bmp390Addr) {
    if (dpsProbe(0x77)) dpsAddr = 0x77;
    else if (dpsProbe(0x76)) dpsAddr = 0x76;
    if (dpsAddr) {
      if (dpsBegin(dpsAddr)) {
        s_dev.dpsAddr = dpsAddr;
        s_dev.dpsOk = true;
        LOG_I("DPS310/368: wykryty pod adresem 0x%02X (temperatura i ciśnienie)", dpsAddr);
        foundCh(CH_DPS_T);
        foundCh(CH_DPS_P);
      } else {
        LOG_W("DPS310/368: układ pod adresem 0x%02X nie odpowiedział na kalibrację", dpsAddr);
        dpsAddr = 0;
      }
    }
  }

  // MS5611 dzieli adresy 0x76/0x77 z BMP/DPS - obsługujemy tylko pierwszy
  // potwierdzony układ, a pozostałe kandydatury zgłaszamy jako pominięte.
  const bool msEnabled = (config.extraF("ths_ms5611", 1.0f) > 0.5f);
  uint8_t msAddr = 0;
  if (!bmp280Addr && !bmp390Addr && !dpsAddr && msEnabled) {
    if (msProbe(0x77)) msAddr = 0x77;
    else if (msProbe(0x76)) msAddr = 0x76;
    if (msAddr) {
      s_dev.msAddr = msAddr;
      s_dev.msOk = true;
      LOG_I("MS5611: wykryty pod adresem 0x%02X (temperatura i ciśnienie)", msAddr);
      foundCh(CH_MS5611_T);
      foundCh(CH_MS5611_P);
    }
  }
  if (bmp280Addr || bmp390Addr || dpsAddr) {
    const char* winName = bmp280Addr ? (s_dev.bmp280Bme ? "BME280" : "BMP280")
                                     : (bmp390Addr ? "BMP388/390" : "DPS310/368");
    if (msEnabled) LOG_I("MS5611: pominięty (adresy 0x76/0x77 obsługuje %s)", winName);
  }

  // --- LPS22HB / LPS25HB / LPS28DFW ---
  uint8_t lpsAddr = 0, lpsKind = 0, lpsId = 0;
  if (lpsProbe(THS_LPS_ADDR1, &lpsKind, &lpsId)) lpsAddr = THS_LPS_ADDR1;
  else if (lpsProbe(THS_LPS_ADDR2, &lpsKind, &lpsId)) lpsAddr = THS_LPS_ADDR2;
  if (lpsAddr) {
    if (lpsBegin(lpsAddr, lpsKind)) {
      s_dev.lpsAddr = lpsAddr;
      s_dev.lpsKind = lpsKind;
      s_dev.lpsOk = true;
      LOG_I("%s: wykryty pod adresem 0x%02X, WHO_AM_I 0x%02X",
            (lpsKind == 0) ? "LPS22HB" : ((lpsKind == 1) ? "LPS25HB" : "LPS28DFW"), lpsAddr, lpsId);
      if (lpsKind == 0) {
        foundCh(CH_LPS22_T);
        foundCh(CH_LPS22_P);
      } else if (lpsKind == 1) {
        foundCh(CH_LPS25_P);
      } else {
        foundCh(CH_LPS28_P);
      }
    } else {
      LOG_W("LPS22/25/28: układ pod adresem 0x%02X nie odpowiedział na konfigurację", lpsAddr);
      lpsAddr = 0;
    }
  }

  // --- MPL3115A2 ---
  if (mplProbe(THS_MPL_ADDR)) {
    if (mplBegin(THS_MPL_ADDR)) {
      s_dev.mplAddr = THS_MPL_ADDR;
      s_dev.mplOk = true;
      LOG_I("MPL3115A2: wykryty pod adresem 0x%02X (temperatura i ciśnienie)", THS_MPL_ADDR);
      foundCh(CH_MPL_T);
      foundCh(CH_MPL_P);
    } else {
      LOG_W("MPL3115A2: układ pod adresem 0x%02X nie odpowiedział na konfigurację", THS_MPL_ADDR);
    }
  }

  // --- SDP810/SDP800: 0x25, potem 0x26 ---
  uint8_t sdpAddr = 0;
  if (sdpProbe(THS_SDP_ADDR1)) sdpAddr = THS_SDP_ADDR1;
  else if (sdpProbe(THS_SDP_ADDR2)) sdpAddr = THS_SDP_ADDR2;
  if (sdpAddr) {
    s_dev.sdpAddr = sdpAddr;
    s_dev.sdpOk = true;
    LOG_I("SDP810/800: wykryty pod adresem 0x%02X (ciśnienie różnicowe)", sdpAddr);
    foundCh(CH_SDP_DP);
  }
}

// =============================================================
//  Odczyt - tylko potwierdzone układy
// =============================================================
static void readImpl() {
  if (s_dev.shtOk) shtRead();
  if (s_dev.ahtOk) ahtRead();
  if (s_dev.hdcOk) hdcRead();
  if (s_dev.siOk) siRead();
  if (s_dev.tmpOk) tmpRead();
  if (s_dev.mcpOk) mcpRead();
  if (s_dev.bmp280Ok) bmp280Read();
  if (s_dev.bmp390Ok) bmp390Read();
  if (s_dev.dpsOk) dpsRead();
  if (s_dev.lpsOk) lpsRead();
  if (s_dev.mplOk) mplRead();
  if (s_dev.msOk) msRead();
  if (s_dev.sdpOk) sdpRead();
}

// ---------- Rejestracja modułu ----------
const DrvModule drvThs = { "T/H/P", CHANS, NCHANS, beginImpl, readImpl };
