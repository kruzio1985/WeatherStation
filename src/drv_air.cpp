/* =============================================================================
 * Stacja Pogody - stacja meteorologiczna na ESP32-S3 WROOM-1 (N16R8)
 * Copyright (c) 2026 kruzio1985 - https://github.com/kruzio1985
 * Licencja: Stacja Pogody Non-Commercial License 1.0 (plik LICENSE)
 * Użytek niekomercyjny. Kontakt: kruzio1985@users.noreply.github.com
 * =============================================================================
 */
#include "drv_air.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "i2c_util.h"
#include "syslog.h"

// =============================================================
//  Czujniki jakości powietrza (I2C, ręcznie - bez bibliotek
//  Adafruit_*/SparkFun_*, tylko Wire).
//
//  Obsługiwane układy:
//    * SCD40/SCD41 (SCD4x)   0x62     0x21B1 start ciągły, 0xE4B8 stan danych,
//                                     0xEC05 odczyt (CO2, T, RH)
//    * SCD30                 0x61     0xD100 wersja, 0x0010 start ciągły,
//                                     0x0202 stan danych, 0x0300 odczyt
//    * SGP30                 0x58     0x2003 iaq_init, 0x2008 pomiar,
//                                     0x202F feature set, 0x2061 kompensacja RH
//    * SGP40                 0x59     0x260F pomiar surowy (SRAW_VOC)
//    * SGP41                 0x59     0x2612 kondycjonowanie,
//                                     0x2619 pomiar (VOC + NOx)
//    * CCS811                0x5A/0x5B  HW_ID 0x20 = 0x8x, tryb 1 s,
//                                     0x02 wynik algorytmu (eCO2 + TVOC)
//    * ENS160                0x52/0x53  PART_ID 0x00 = 0x0160, tryb 2 (standard),
//                                     AQI/TVOC/eCO2 + kompensacja T/RH
//
//  SPS30 / SEN5x / SEN50 / SEN55 (0x69) oraz pyły obsługuje już
//  src/drv_pm.cpp - tutaj ich świadomie nie ma, żeby nie dublować
//  sterownika na tej samej magistrali.
//
//  Adresy dzielone przez różne układy (np. 0x59 dla SGP40 i SGP41)
//  rozstrzygamy po odpowiedzi na komendę + CRC.  Wykrycie sprzętu bez
//  potwierdzenia = cisza: żadnych kanałów i żadnych logów W/E.
//  Układy z trybem ciągłym (SCD4x, SCD30, SGP30, CCS811, ENS160)
//  uruchamiamy w begin(), a w read() tylko odczytujemy wynik.
// =============================================================

// ---------- Kanały ----------
enum {
  CH_CO2 = 0, CH_CO2_T, CH_CO2_H,
  CH_SGP30_ECO2, CH_SGP30_TVOC,
  CH_SGP40_VOC, CH_SGP41_NOX,
  CH_CCS_ECO2, CH_CCS_TVOC,
  CH_ENS_AQI, CH_ENS_ECO2, CH_ENS_TVOC, CH_ENS_T, CH_ENS_H
};

static const ChanDef CHANS[] = {
  { "aq_co2",        "CO₂ (SCD4x)",          "ppm", 0, "in", "carbon_dioxide",             "ppm", "mdi:molecule-co2" },
  { "aq_co2_t",      "Temperatura (SCD4x)",  "°C",  1, "in", "temperature",                "°C",  "mdi:thermometer" },
  { "aq_co2_h",      "Wilgotność (SCD4x)",   "%",   1, "in", "humidity",                   "%",   "mdi:water-percent" },
  { "aq_sgp30_eco2", "eCO₂ (SGP30)",         "ppm", 0, "in", "carbon_dioxide",             "ppm", "mdi:molecule-co2" },
  { "aq_sgp30_tvoc", "TVOC (SGP30)",         "ppb", 0, "in", "volatile_organic_compounds_parts", "ppb", "mdi:chemical-weapon" },
  { "aq_sgp40_voc",  "VOC (SGP40)",          "",    0, "in", "",                           "",    "mdi:chemical-weapon" },
  { "aq_sgp41_nox",  "NOx (SGP41)",          "",    0, "in", "",                           "",    "mdi:chemical-weapon" },
  { "aq_ccs_eco2",   "eCO₂ (CCS811)",        "ppm", 0, "in", "carbon_dioxide",             "ppm", "mdi:molecule-co2" },
  { "aq_ccs_tvoc",   "TVOC (CCS811)",        "ppb", 0, "in", "volatile_organic_compounds_parts", "ppb", "mdi:chemical-weapon" },
  { "aq_ens_aqi",    "AQI (ENS160)",         "",    0, "in", "aqi",                        "",    "mdi:air-filter" },
  { "aq_ens_eco2",   "eCO₂ (ENS160)",        "ppm", 0, "in", "carbon_dioxide",             "ppm", "mdi:molecule-co2" },
  { "aq_ens_tvoc",   "TVOC (ENS160)",        "ppb", 0, "in", "volatile_organic_compounds_parts", "ppb", "mdi:chemical-weapon" },
  { "aq_ens_t",      "Temperatura (ENS160)", "°C",  1, "in", "temperature",                "°C",  "mdi:thermometer" },
  { "aq_ens_h",      "Wilgotność (ENS160)",  "%",   1, "in", "humidity",                   "%",   "mdi:water-percent" }
};

static const uint16_t NCHANS = sizeof(CHANS) / sizeof(CHANS[0]);

// ---------- Bramki wartości (publikujemy tylko rozsądne pomiary) ----------
#define AIR_CO2_MAX   60000.0f   // ppm
#define AIR_T_MIN     (-40.0f)   // °C
#define AIR_T_MAX     (85.0f)
#define AIR_RH_MIN    (0.0f)     // %
#define AIR_RH_MAX    (100.0f)

// ---------- Adresy ----------
#define AIR_SCD4X_ADDR  0x62
#define AIR_SCD30_ADDR  0x61
#define AIR_SGP30_ADDR  0x58
#define AIR_SGP4X_ADDR  0x59      // SGP40 i SGP41 dzielą adres
#define AIR_CCS_ADDR1   0x5A
#define AIR_CCS_ADDR2   0x5B
#define AIR_ENS_ADDR1   0x52
#define AIR_ENS_ADDR2   0x53

// ---------- SCD4x (SCD40/SCD41) ----------
#define SCD4X_CMD_START_CONT 0x21B1   // pomiar ciągły (co 5 s)
#define SCD4X_CMD_READ_MEAS  0xEC05   // odczyt wyniku (3 słowa)
#define SCD4X_CMD_DATA_READY 0xE4B8   // status gotowości danych
#define SCD4X_CMD_SET_ALT    0x2427   // wysokość n.p.m. (tylko w trybie bezczynnym)

// ---------- SCD30 ----------
#define SCD30_CMD_START_CONT 0x0010   // arg: ciśnienie otoczenia [mBar], 0 = bez
#define SCD30_CMD_DATA_READY 0x0202
#define SCD30_CMD_READ_MEAS  0x0300
#define SCD30_CMD_FW_VERSION 0xD100
#define SCD30_CMD_SET_ALT    0x5102

// ---------- SGP30 ----------
#define SGP30_CMD_IAQ_INIT    0x2003
#define SGP30_CMD_MEASURE_IAQ 0x2008
#define SGP30_CMD_FEATURE_SET 0x202F
#define SGP30_CMD_SET_HUM     0x2061   // arg: wilgotność bezwzględna [g/m3] * 256

// ---------- SGP40 / SGP41 ----------
#define SGP40_CMD_MEASURE_RAW 0x260F   // SGP40: pomiar surowy VOC
#define SGP41_CMD_CONDITION   0x2612   // SGP41: kondycjonowanie (tylko 0x59 = SGP41)
#define SGP41_CMD_MEASURE_RAW 0x2619   // SGP41: pomiar surowy VOC + NOx

// ---------- CCS811 ----------
#define CCS_REG_STATUS     0x00
#define CCS_REG_MODE       0x01
#define CCS_REG_ALG_RESULT 0x02
#define CCS_REG_HW_ID      0x20
#define CCS_CMD_APP_START  0xF4
#define CCS_MODE_1S        0x10

// ---------- ENS160 ----------
#define ENS_REG_PART_ID     0x00
#define ENS_REG_OPMODE      0x10
#define ENS_REG_TEMP_IN     0x13   // wejście kompensacji T (układ nie ma własnego T/RH)
#define ENS_REG_RH_IN       0x15   // wejście kompensacji RH
#define ENS_REG_DATA_STATUS 0x20
#define ENS_REG_DATA_AQI    0x21
#define ENS_REG_DATA_TVOC   0x22
#define ENS_REG_DATA_ECO2   0x24
#define ENS_REG_DATA_T      0x30   // T z czujnika towarzyszącego / wewnętrzny
#define ENS_REG_DATA_RH     0x32
#define ENS_OPMODE_STD      0x02
#define ENS_ST_NEWDAT       0x02
#define ENS_ST_VALID_MASK   0x0C   // >>2: 0 = dane ważne, inne = rozgrzewanie/start
#define ENS_ST_STATER       0x40
#define ENS_ST_STATAS       0x80

// ---------- Odstępy między odczytami [ms] ----------
#define AIR_MS_SCD4X       5000   // SCD4x mierzy z okresem 5 s
#define AIR_MS_DEFAULT     1000   // SCD30, SGP30, SGP4x, CCS811, ENS160 (1 s)
#define AIR_MS_SGP30_BOOT  15000  // pierwsze 15 s po iaq_init to wartości zastępcze
#define AIR_SGP41_COND     10     // kondycjonowanie SGP41: 10 wywołań (wzorzec Sensirion)

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
static void pubCo2(uint8_t ch, float ppm) {
  if (isnan(ppm) || !(ppm > 0.0f && ppm < AIR_CO2_MAX)) { clearCh(ch); return; }
  publishCh(ch, ppm);
}

static void pubT(uint8_t ch, float c) {
  if (isnan(c) || !(c > AIR_T_MIN && c < AIR_T_MAX)) { clearCh(ch); return; }
  publishCh(ch, c);
}

static void pubH(uint8_t ch, float rh) {
  if (isnan(rh) || !(rh >= AIR_RH_MIN && rh <= AIR_RH_MAX)) { clearCh(ch); return; }
  publishCh(ch, rh);
}

// ---------- Wykryte układy ----------
static bool s_scd4x = false;
static bool s_scd30 = false;
static bool s_sgp30 = false;
static bool s_sgp40 = false;
static bool s_sgp41 = false;
static uint8_t s_ccsAddr = 0;      // 0 = brak CCS811
static uint8_t s_ensAddr = 0;      // 0 = brak ENS160

// ---------- Znaczniki czasu odczytu / rozgrzewania ----------
static uint32_t s_tScd4x = 0, s_tScd30 = 0, s_tSgp30 = 0, s_tSgp4x = 0, s_tCcs = 0, s_tEns = 0;
static uint32_t s_tSgp30Init = 0;  // millis() po iaq_init (15 s wartości zastępczych)
static uint8_t s_sgp4xPhase = 0;   // 0 = wyślij komendę, 1 = odczytaj wynik (pomiar trwa 50 ms)
static bool s_sgp4xCond = false;   // true = wysłano 0x2612 (wynik to 1 słowo)
static uint8_t s_sgp41Cond = 0;    // licznik wywołań kondycjonowania SGP41

// ---------- Ostatni wiarygodny pomiar T/RH (kompensacja SGP4x i ENS160) ----------
static bool s_refValid = false;
static float s_refT = 25.0f;
static float s_refH = 50.0f;
static float s_lastCompT = -999.0f, s_lastCompH = -999.0f;
static float s_tOff = 0.0f;        // korekta temperatury ENS160 (config: aq_ens_t_off)

// =============================================================
//  Pomocniki Sensirion: komendy 16-bit, dane jako "2 bajty + CRC8",
//  CRC poly 0x31, init 0xFF (i2cuCrc8 / i2cuCheckCrc3).
// =============================================================
static bool senCmd(uint8_t addr, uint16_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

// Komenda z jednym argumentem 16-bit - argument też w formacie "2 bajty + CRC"
static bool senCmdArg(uint8_t addr, uint16_t cmd, uint16_t a0) {
  uint8_t a[3] = { (uint8_t)(a0 >> 8), (uint8_t)(a0 & 0xFF), 0 };
  a[2] = i2cuCrc8(a, 2);
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  Wire.write(a, 3);
  return Wire.endTransmission() == 0;
}

// Komenda z dwoma argumentami 16-bit (SGP4x: RH i T w "ticks")
static bool senCmdArg2(uint8_t addr, uint16_t cmd, uint16_t a0, uint16_t a1) {
  uint8_t a[6] = { (uint8_t)(a0 >> 8), (uint8_t)(a0 & 0xFF), 0,
                   (uint8_t)(a1 >> 8), (uint8_t)(a1 & 0xFF), 0 };
  a[2] = i2cuCrc8(a, 2);
  a[5] = i2cuCrc8(a + 3, 2);
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  Wire.write(a, 6);
  return Wire.endTransmission() == 0;
}

// Odczyt N słów (każde 2 bajty + CRC), bez CRC w buforze wynikowym
static bool senReadWords(uint8_t addr, uint16_t* w, uint8_t n) {
  uint8_t raw[24];
  if (n == 0 || n > 8) return false;
  if (!i2cuRead(addr, raw, (size_t)n * 3)) return false;
  if (!i2cuCheckCrc3(raw, n)) return false;
  for (uint8_t i = 0; i < n; i++) {
    w[i] = (uint16_t)(((uint16_t)raw[i * 3] << 8) | raw[i * 3 + 1]);
  }
  return true;
}

// To samo z jednym ponowieniem - układy potrzebują chwili na przygotowanie danych
static bool senReadWords2(uint8_t addr, uint16_t* w, uint8_t n) {
  if (senReadWords(addr, w, n)) return true;
  delay(3);
  return senReadWords(addr, w, n);
}

// Float32 big-endian złożony z dwóch słów (SCD30)
static float beFloat(uint16_t hi, uint16_t lo) {
  uint32_t u = ((uint32_t)hi << 16) | (uint32_t)lo;
  float f;
  memcpy(&f, &u, sizeof(f));
  return f;
}

// Wartości kompensacji dla SGP4x: RH [%] i T [°C] jako 16-bitowe "ticks"
static uint16_t rhTicks(float rh) {
  if (isnan(rh)) rh = 50.0f;
  if (rh < 0.0f) rh = 0.0f;
  if (rh > 100.0f) rh = 100.0f;
  return (uint16_t)((rh * 65535.0f) / 100.0f + 0.5f);
}

static uint16_t tTicks(float c) {
  if (isnan(c)) c = 25.0f;
  if (c < -45.0f) c = -45.0f;
  if (c > 130.0f) c = 130.0f;
  return (uint16_t)(((c + 45.0f) * 65535.0f) / 175.0f + 0.5f);
}

// =============================================================
//  SCD40 / SCD41 / SCD30 (CO2, temperatura, wilgotność)
// =============================================================
static void scd4xRead() {
  uint32_t now = millis();
  if (now - s_tScd4x < AIR_MS_SCD4X) return;     // dane odświeżane co 5 s
  s_tScd4x = now;

  uint16_t st = 0;
  if (!senCmd(AIR_SCD4X_ADDR, SCD4X_CMD_DATA_READY) || !senReadWords2(AIR_SCD4X_ADDR, &st, 1)) {
    clearCh(CH_CO2); clearCh(CH_CO2_T); clearCh(CH_CO2_H);
    return;
  }
  if ((st & 0x07FF) == 0) return;                // pomiar jeszcze nie gotowy - nic nie zmieniamy

  uint16_t w[3];
  if (!senCmd(AIR_SCD4X_ADDR, SCD4X_CMD_READ_MEAS) || !senReadWords2(AIR_SCD4X_ADDR, w, 3)) {
    clearCh(CH_CO2); clearCh(CH_CO2_T); clearCh(CH_CO2_H);
    return;
  }

  float t = -45.0f + 175.0f * (float)w[1] / 65536.0f;
  float h = 100.0f * (float)w[2] / 65536.0f;
  pubCo2(CH_CO2, (float)w[0]);
  pubT(CH_CO2_T, t);
  pubH(CH_CO2_H, h);
  if (!isnan(t) && t > AIR_T_MIN && t < AIR_T_MAX) { s_refValid = true; s_refT = t; s_refH = h; }
}

static void scd30Read() {
  uint32_t now = millis();
  if (now - s_tScd30 < AIR_MS_DEFAULT) return;
  s_tScd30 = now;

  uint16_t ready = 0;
  if (!senCmd(AIR_SCD30_ADDR, SCD30_CMD_DATA_READY) || !senReadWords2(AIR_SCD30_ADDR, &ready, 1)) {
    clearCh(CH_CO2); clearCh(CH_CO2_T); clearCh(CH_CO2_H);
    return;
  }
  if (ready != 1) return;                        // pomiar jeszcze nie gotowy

  uint16_t w[6];
  if (!senCmd(AIR_SCD30_ADDR, SCD30_CMD_READ_MEAS) || !senReadWords2(AIR_SCD30_ADDR, w, 6)) {
    clearCh(CH_CO2); clearCh(CH_CO2_T); clearCh(CH_CO2_H);
    return;
  }

  float co2 = beFloat(w[0], w[1]);
  float t   = beFloat(w[2], w[3]);
  float h   = beFloat(w[4], w[5]);
  pubCo2(CH_CO2, co2);
  pubT(CH_CO2_T, t);
  pubH(CH_CO2_H, h);
  if (!isnan(t) && t > AIR_T_MIN && t < AIR_T_MAX) { s_refValid = true; s_refT = t; s_refH = h; }
}

// =============================================================
//  SGP30 (eCO2 + TVOC)
// =============================================================
static void sgp30Read() {
  uint32_t now = millis();
  if (now - s_tSgp30 < AIR_MS_DEFAULT) return;
  s_tSgp30 = now;

  uint16_t w[2];
  if (!senCmd(AIR_SGP30_ADDR, SGP30_CMD_MEASURE_IAQ) || !senReadWords2(AIR_SGP30_ADDR, w, 2)) {
    clearCh(CH_SGP30_ECO2); clearCh(CH_SGP30_TVOC);
    return;
  }
  // Po iaq_init układ zwraca przez pierwsze 15 s wartości zastępcze (400 ppm / 0 ppb)
  if (millis() - s_tSgp30Init < AIR_MS_SGP30_BOOT) return;

  publishCh(CH_SGP30_ECO2, (float)w[0]);
  publishCh(CH_SGP30_TVOC, (float)w[1]);
}

// =============================================================
//  SGP40 / SGP41 (surowy VOC, SGP41 dodatkowo NOx)
//
//  Pomiar trwa ~50 ms, a read() nie może blokować magistrali, więc
//  pracujemy w dwóch cyklach: w jednym wysyłamy komendę, w następnym
//  (5 s później) odczytujemy wynik.  SGP41 przed pierwszym pomiarem
//  wymaga kondycjonowania (0x2612 - 10 wywołań, wzorzec Sensirion).
// =============================================================
static void sgp4xRead() {
  uint32_t now = millis();
  if (now - s_tSgp4x < AIR_MS_DEFAULT) return;
  s_tSgp4x = now;

  float t = s_refValid ? s_refT : 25.0f;
  float h = s_refValid ? s_refH : 50.0f;
  uint16_t rh = rhTicks(h);
  uint16_t tt = tTicks(t);

  if (s_sgp4xPhase == 0) {
    bool cond = s_sgp41 && (s_sgp41Cond < AIR_SGP41_COND);
    uint16_t cmd;
    if (!s_sgp41)     cmd = SGP40_CMD_MEASURE_RAW;
    else if (cond)    cmd = SGP41_CMD_CONDITION;
    else              cmd = SGP41_CMD_MEASURE_RAW;

    if (!senCmdArg2(AIR_SGP4X_ADDR, cmd, rh, tt)) {
      clearCh(CH_SGP40_VOC); if (s_sgp41) clearCh(CH_SGP41_NOX);
      return;
    }
    s_sgp4xCond = cond;
    s_sgp4xPhase = 1;
    return;
  }

  // Faza 2: odczyt wyniku pomiaru wysłanego w poprzednim cyklu
  s_sgp4xPhase = 0;

  if (s_sgp4xCond) {
    uint16_t v = 0;
    if (!senReadWords2(AIR_SGP4X_ADDR, &v, 1)) {           // wynik kondycjonowania
      clearCh(CH_SGP40_VOC); clearCh(CH_SGP41_NOX);
      return;
    }
    s_sgp41Cond++;
    return;                                                // w trakcie kondycjonowania nie publikujemy
  }

  uint16_t w[2];
  if (s_sgp41) {
    if (!senReadWords2(AIR_SGP4X_ADDR, w, 2)) {
      clearCh(CH_SGP40_VOC); clearCh(CH_SGP41_NOX);
      return;
    }
    publishCh(CH_SGP40_VOC, (float)w[0]);   // SGP41: VOC wchodzi na kanał VOC (0x59 jest tylko jeden)
    publishCh(CH_SGP41_NOX, (float)w[1]);
  } else {
    if (!senReadWords2(AIR_SGP4X_ADDR, w, 1)) { clearCh(CH_SGP40_VOC); return; }
    publishCh(CH_SGP40_VOC, (float)w[0]);
  }
}

// =============================================================
//  CCS811 (eCO2 + TVOC, tryb 1 s)
// =============================================================
static void ccsRead() {
  uint32_t now = millis();
  if (now - s_tCcs < AIR_MS_DEFAULT) return;
  s_tCcs = now;

  uint8_t st = 0;
  if (!i2cuReadReg(s_ccsAddr, CCS_REG_STATUS, &st, 1)) {
    clearCh(CH_CCS_ECO2); clearCh(CH_CCS_TVOC);
    return;
  }
  if (st & 0x01) { clearCh(CH_CCS_ECO2); clearCh(CH_CCS_TVOC); return; }   // flaga ERROR
  if (!(st & 0x08)) return;                                               // dane jeszcze nie gotowe

  uint8_t b[4];
  if (!i2cuReadReg(s_ccsAddr, CCS_REG_ALG_RESULT, b, 4)) {
    clearCh(CH_CCS_ECO2); clearCh(CH_CCS_TVOC);
    return;
  }
  publishCh(CH_CCS_ECO2, (float)(((uint16_t)b[0] << 8) | b[1]));
  publishCh(CH_CCS_TVOC, (float)(((uint16_t)b[2] << 8) | b[3]));
}

// =============================================================
//  ENS160 (AQI, TVOC, eCO2 + kompensacja T/RH)
//
//  ENS160 nie ma własnego czujnika T/RH - rejestry 0x13/0x15
//  (TEMP_IN/RH_IN) to WEJŚCIE kompensacji.  Wpisujemy tam pomiar
//  z czujnika odniesienia (SCD4x/SCD30), a te same rejestry
//  odczytujemy dla kanałów aq_ens_t/aq_ens_h.  Gdy nic nie było
//  wpisane (same wartości 0/0xFFFF), próbujemy 0x30/0x32
//  (czujnik towarzyszący).  Brak sensownego T/RH = kanał bez wartości.
// =============================================================
static bool ensRead16(uint8_t reg, uint16_t& v) {
  uint8_t b[2];
  if (!i2cuReadReg(s_ensAddr, reg, b, 2)) return false;
  v = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
  return true;
}

static bool ensWriteComp(float t, float h) {
  uint16_t ti = (uint16_t)((t + 273.15f) * 64.0f);
  uint16_t hi = (uint16_t)(h * 512.0f);
  Wire.beginTransmission(s_ensAddr);
  Wire.write(ENS_REG_TEMP_IN);
  Wire.write((uint8_t)(ti & 0xFF));
  Wire.write((uint8_t)(ti >> 8));
  Wire.write((uint8_t)(hi & 0xFF));
  Wire.write((uint8_t)(hi >> 8));
  return Wire.endTransmission() == 0;
}

static void ensReadTRH() {
  uint16_t tRaw = 0, hRaw = 0;
  bool okT = ensRead16(ENS_REG_TEMP_IN, tRaw);
  bool okH = ensRead16(ENS_REG_RH_IN, hRaw);
  if (!okT || !okH || tRaw == 0 || tRaw == 0xFFFF) {
    // Nic nie wpisano - spróbuj danych z czujnika T/RH dołączonego do ENS160
    tRaw = 0; hRaw = 0;
    okT = ensRead16(ENS_REG_DATA_T, tRaw);
    okH = ensRead16(ENS_REG_DATA_RH, hRaw);
    if (!okT || !okH || tRaw == 0 || tRaw == 0xFFFF) {
      clearCh(CH_ENS_T); clearCh(CH_ENS_H);      // brak jakiegokolwiek źródła T/RH
      return;
    }
  }
  float t = (float)tRaw / 64.0f - 273.15f + s_tOff;
  float h = (float)hRaw / 512.0f;
  pubT(CH_ENS_T, t);
  pubH(CH_ENS_H, h);
}

static void ensRead() {
  uint32_t now = millis();
  if (now - s_tEns < AIR_MS_DEFAULT) return;
  s_tEns = now;

  // Kompensacja T/RH - wpisujemy tylko przy realnej zmianie
  if (s_refValid) {
    if (fabsf(s_refT - s_lastCompT) > 0.5f || fabsf(s_refH - s_lastCompH) > 1.0f) {
      if (ensWriteComp(s_refT, s_refH)) { s_lastCompT = s_refT; s_lastCompH = s_refH; }
    }
  }

  uint8_t st = 0;
  if (!i2cuReadReg(s_ensAddr, ENS_REG_DATA_STATUS, &st, 1)) {
    clearCh(CH_ENS_AQI); clearCh(CH_ENS_ECO2); clearCh(CH_ENS_TVOC);
    clearCh(CH_ENS_T); clearCh(CH_ENS_H);
    return;
  }
  if (st & ENS_ST_STATER) return;                        // błąd układu - nie publikujemy
  if ((st & ENS_ST_VALID_MASK) != 0) return;             // rozgrzewanie / start / dane nieważne

  uint16_t aqi = 0, tvoc = 0, eco2 = 0;
  if (!ensRead16(ENS_REG_DATA_AQI, aqi) || !ensRead16(ENS_REG_DATA_TVOC, tvoc) ||
      !ensRead16(ENS_REG_DATA_ECO2, eco2)) {
    clearCh(CH_ENS_AQI); clearCh(CH_ENS_ECO2); clearCh(CH_ENS_TVOC);
    return;
  }
  publishCh(CH_ENS_AQI, (float)(aqi & 0x07));
  pubCo2(CH_ENS_ECO2, (float)eco2);
  publishCh(CH_ENS_TVOC, (float)tvoc);

  ensReadTRH();
}

// =============================================================
//  Wykrywanie sprzętu (begin)
// =============================================================
static void beginImpl(const DrvSink& s) {
  s_sink = s;

  // Stan startowy (begin może być wywołany ponownie po resecie magistrali)
  s_scd4x = s_scd30 = s_sgp30 = s_sgp40 = s_sgp41 = false;
  s_ccsAddr = 0; s_ensAddr = 0;
  s_refValid = false; s_refT = 25.0f; s_refH = 50.0f;
  s_lastCompT = -999.0f; s_lastCompH = -999.0f;
  s_tScd4x = s_tScd30 = s_tSgp30 = s_tSgp4x = s_tCcs = s_tEns = 0;
  s_sgp4xPhase = 0; s_sgp4xCond = false; s_sgp41Cond = 0;
  s_tSgp30Init = millis();

  // Kalibracja z konfiguracji (bez zapisu do NVS)
  s_tOff = config.extraF("aq_ens_t_off", 0.0f);
  float alt = config.extraF("aq_scd_alt", 0.0f);
  if (isnan(alt) || alt < 1.0f || alt > 3000.0f) alt = 0.0f;   // 0 = bez kalibracji wysokością
  float ah = config.extraF("aq_sgp30_ah", 0.0f);
  if (isnan(ah) || ah <= 0.0f || ah > 300.0f) ah = 0.0f;       // 0 = bez kompensacji wilgotności

  // ---------- SCD40 / SCD41 (SCD4x, 0x62) ----------
  if (i2cuPresent(AIR_SCD4X_ADDR)) {
    uint16_t st = 0;
    // Potwierdzenie: odpowiedź na komendę statusu + poprawne CRC
    if (senCmd(AIR_SCD4X_ADDR, SCD4X_CMD_DATA_READY) && senReadWords2(AIR_SCD4X_ADDR, &st, 1)) {
      s_scd4x = true;
      // Wysokość n.p.m. ustawiamy przed startem pomiaru (komenda działa tylko w bezczynności)
      if (alt > 0.0f) senCmdArg(AIR_SCD4X_ADDR, SCD4X_CMD_SET_ALT, (uint16_t)(alt + 0.5f));
      // Pomiar ciągły co 5 s - NACK, gdy układ już mierzy (np. wystartowany w sensors.cpp)
      senCmd(AIR_SCD4X_ADDR, SCD4X_CMD_START_CONT);
      s_tScd4x = millis();
      LOG_I("SCD4x: wykryty pod adresem 0x%02X (CO2, temperatura, wilgotność)", AIR_SCD4X_ADDR);
      foundCh(CH_CO2); foundCh(CH_CO2_T); foundCh(CH_CO2_H);
    }
  }

  // ---------- SCD30 (0x61) ----------
  if (i2cuPresent(AIR_SCD30_ADDR)) {
    uint16_t fw = 0;
    if (senCmd(AIR_SCD30_ADDR, SCD30_CMD_FW_VERSION) && senReadWords2(AIR_SCD30_ADDR, &fw, 1)) {
      s_scd30 = true;
      if (alt > 0.0f) senCmdArg(AIR_SCD30_ADDR, SCD30_CMD_SET_ALT, (uint16_t)(alt + 0.5f));
      // Start pomiaru ciągłego (argument = ciśnienie otoczenia [mBar], 0 = bez kompensacji)
      senCmdArg(AIR_SCD30_ADDR, SCD30_CMD_START_CONT, 0);
      s_tScd30 = millis();
      LOG_I("SCD30: wykryty pod adresem 0x%02X, FW 0x%04X (CO2, temperatura, wilgotność)",
            AIR_SCD30_ADDR, fw);
      foundCh(CH_CO2); foundCh(CH_CO2_T); foundCh(CH_CO2_H);
    }
  }

  // ---------- SGP30 (0x58) ----------
  if (i2cuPresent(AIR_SGP30_ADDR)) {
    uint16_t feat = 0, w[2];
    if (senCmd(AIR_SGP30_ADDR, SGP30_CMD_FEATURE_SET) && senReadWords2(AIR_SGP30_ADDR, &feat, 1)) {
      senCmd(AIR_SGP30_ADDR, SGP30_CMD_IAQ_INIT);
      delay(10);                                   // iaq_init trwa ~10 ms
      // Potwierdzenie: odpowiedź na pomiar + poprawne CRC (odróżnienie od innych układów)
      if (senCmd(AIR_SGP30_ADDR, SGP30_CMD_MEASURE_IAQ) && senReadWords2(AIR_SGP30_ADDR, w, 2)) {
        s_sgp30 = true;
        s_tSgp30Init = millis();
        if (ah > 0.0f) senCmdArg(AIR_SGP30_ADDR, SGP30_CMD_SET_HUM, (uint16_t)(ah * 256.0f + 0.5f));
        LOG_I("SGP30: wykryty pod adresem 0x%02X, feature set 0x%04X (eCO2, TVOC)",
              AIR_SGP30_ADDR, feat);
        foundCh(CH_SGP30_ECO2); foundCh(CH_SGP30_TVOC);
      }
    }
  }

  // ---------- SGP40 / SGP41 (0x59) ----------
  if (i2cuPresent(AIR_SGP4X_ADDR)) {
    uint16_t rh = rhTicks(50.0f), tt = tTicks(25.0f), w[2];
    // SGP41: komenda 0x2619 (surowe VOC + NOx)
    if (senCmdArg2(AIR_SGP4X_ADDR, SGP41_CMD_MEASURE_RAW, rh, tt)) {
      delay(50);                                   // czas pomiaru SGP41
      if (senReadWords2(AIR_SGP4X_ADDR, w, 2)) s_sgp41 = true;
    }
    // SGP40: komenda 0x260F (surowe VOC)
    if (!s_sgp41 && senCmdArg2(AIR_SGP4X_ADDR, SGP40_CMD_MEASURE_RAW, rh, tt)) {
      delay(50);                                   // czas pomiaru SGP40
      uint16_t v = 0;
      if (senReadWords2(AIR_SGP4X_ADDR, &v, 1)) s_sgp40 = true;
    }
    // SGP41 bez kondycjonowania: 0x2619 nie odpowie, ale 0x2612 tak (SGP40 jej nie zna)
    if (!s_sgp41 && !s_sgp40 && senCmdArg2(AIR_SGP4X_ADDR, SGP41_CMD_CONDITION, rh, tt)) {
      delay(50);
      uint16_t v = 0;
      if (senReadWords2(AIR_SGP4X_ADDR, &v, 1)) { s_sgp41 = true; s_sgp41Cond = 1; }
    }

    if (s_sgp41) {
      LOG_I("SGP41: wykryty pod adresem 0x%02X (VOC, NOx)", AIR_SGP4X_ADDR);
      foundCh(CH_SGP40_VOC); foundCh(CH_SGP41_NOX);
    } else if (s_sgp40) {
      LOG_I("SGP40: wykryty pod adresem 0x%02X (VOC)", AIR_SGP4X_ADDR);
      foundCh(CH_SGP40_VOC);
    }
    s_tSgp4x = millis();
  }

  // ---------- CCS811 (0x5A / 0x5B) ----------
  static const uint8_t ccsCand[2] = { AIR_CCS_ADDR1, AIR_CCS_ADDR2 };
  for (uint8_t i = 0; i < 2 && !s_ccsAddr; i++) {
    uint8_t hw = 0, st = 0;
    if (!i2cuPresent(ccsCand[i])) continue;
    if (!i2cuReadReg(ccsCand[i], CCS_REG_HW_ID, &hw, 1)) continue;
    if ((hw & 0xF0) != 0x80) continue;             // ID CCS811 = 0x8x
    if (!i2cuReadReg(ccsCand[i], CCS_REG_STATUS, &st, 1)) continue;
    if (!(st & 0x80)) {                            // FW_MODE = 0 -> tryb bootloadera
      if (!(st & 0x10)) continue;                  // brak ważnej aplikacji
      if (!i2cuWrite8(ccsCand[i], CCS_CMD_APP_START)) continue;
      delay(100);                                  // start aplikacji
      if (!i2cuReadReg(ccsCand[i], CCS_REG_STATUS, &st, 1)) continue;
      if (!(st & 0x80)) continue;
    }
    if (!i2cuWriteReg(ccsCand[i], CCS_REG_MODE, CCS_MODE_1S)) continue;
    delay(20);
    s_ccsAddr = ccsCand[i];
    s_tCcs = millis();
    LOG_I("CCS811: wykryty pod adresem 0x%02X, HW_ID 0x%02X (eCO2, TVOC)", s_ccsAddr, hw);
    foundCh(CH_CCS_ECO2); foundCh(CH_CCS_TVOC);
  }

  // ---------- ENS160 (0x52 / 0x53) ----------
  static const uint8_t ensCand[2] = { AIR_ENS_ADDR1, AIR_ENS_ADDR2 };
  for (uint8_t i = 0; i < 2 && !s_ensAddr; i++) {
    uint8_t id[2];
    if (!i2cuPresent(ensCand[i])) continue;
    if (!i2cuReadReg(ensCand[i], ENS_REG_PART_ID, id, 2)) continue;
    uint16_t part = (uint16_t)(id[0] | ((uint16_t)id[1] << 8));
    if (part != 0x0160) continue;
    if (!i2cuWriteReg(ensCand[i], ENS_REG_OPMODE, ENS_OPMODE_STD)) continue;
    delay(20);
    s_ensAddr = ensCand[i];
    s_tEns = millis();
    LOG_I("ENS160: wykryty pod adresem 0x%02X, PART_ID 0x%04X (AQI, eCO2, TVOC)",
          s_ensAddr, part);
    foundCh(CH_ENS_AQI); foundCh(CH_ENS_ECO2); foundCh(CH_ENS_TVOC);
    foundCh(CH_ENS_T); foundCh(CH_ENS_H);
  }
}

// =============================================================
//  Odczyt cykliczny (bez blokowania magistrali)
// =============================================================
static void readImpl(void) {
  if (s_scd4x) scd4xRead();      // pierwsze - dostarcza T/RH do kompensacji SGP4x/ENS160
  if (s_scd30) scd30Read();
  if (s_sgp30) sgp30Read();
  if (s_sgp40 || s_sgp41) sgp4xRead();
  if (s_ccsAddr) ccsRead();
  if (s_ensAddr) ensRead();
}

// ---------- Rejestracja modułu ----------
const DrvModule drvAir = { "Jakosc powietrza", CHANS, NCHANS, beginImpl, readImpl };
